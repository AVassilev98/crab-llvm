/* 
 * Translate a LLVM function to a CFG language understood by Crab.
 * 
 * Crab supports operations over boolean, integers and
 * pointers. Moreover, Crab supports unidimensional arrays. Arrays are
 * interpreted as sequence of consecutive bytes which are disjoint
 * from each other.
 *
 * The translation of LLVM integer operations (tracked precision = NUM)
 * is pretty straightforward.  LLVM branches are translated to Crab
 * assume and goto statements. The translation also removes phi nodes.
 *
 * If tracked precision is PTR then LLVM pointer operations are
 * translated to Crab pointer operations. This translation is almost
 * one-to-one, except some unsupported cases (see below limitations).
 * 
 * If tracked precision is ARR then the translation is more
 * complex. We use a heap analysis to partition statically memory into
 * disjoint regions. Then, each memory region is mapped to a Crab
 * array and LLVM load/store are translated to array read/write. Some
 * memory regions might not be mapped to Crab arrays because
 * otherwise, the Crab array domains wouldn't be sound (see e.g.,
 * SeaDsaHeapAbstraction).
 * 
 * The translation of function calls is also straigthforward except if
 * tracked precision = ARR. In that case, all functions are
 * _purified_. That is, the translation ensures that functions have no
 * side-effects.
 * 
 * Known limitations of the translation:
 * 
 * - Ignore floating point instructions.
 * - Ignore inttoptr/ptrtoint instructions.
 * - Almost ignore memset/memmove/memcpy
 */

#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include "crab/common/debug.hpp"
#include "crab/common/stats.hpp"
#include "crab/transforms/dce.hpp"
#include "clam/CfgBuilder.hh"
#include "clam/HeapAbstraction.hh"
#include "clam/Support/CFG.hh"
#include "clam/Support/Debug.hh"

#include <algorithm>
#include <unordered_map>

#include <boost/functional/hash_fwd.hpp> // for hash_combine

using namespace llvm;
using namespace crab;
using namespace ikos;
using namespace crab::cfg;

namespace clam {

  // Any integer that cannot be represented by 64 bits is considered a bignum.
  static bool isSignedBigNum(const llvm::APInt& v) {
    unsigned b = v.getBitWidth();
    if (b <= 64) {
      return false;
    } else {
      // if bitwidth > 64 then we check the actual value
      llvm::APInt max(b,
		      llvm::APInt::getSignedMaxValue(64).getSExtValue(),
		      true);
      llvm::APInt min(b,
		      llvm::APInt::getSignedMinValue(64).getSExtValue(),
		      true);
      return (v.sgt(max) || v.slt(min));
    }
  }
  
  typedef typename HeapAbstraction::region_t mem_region_t;
  typedef typename HeapAbstraction::region_vector_t mem_region_vector_t;  
  
  static bool isBool(const llvm::Type *t){
    return (t->isIntegerTy(1));
  }
  
  static bool isBool(const llvm::Value&v) {
    return isBool(v.getType());
  }

  static bool isInteger(const llvm::Type *t) {
    return (t->isIntegerTy() && !isBool(t));
  }
  
  static bool isInteger(const llvm::Value&v) {
    return isInteger(v.getType());
  }

  static bool isPointer(const llvm::Type *t, const CrabBuilderParams &params) {
    return (t->isPointerTy() && params.track_pointers());
  }

  static bool isPointer(const llvm::Value&v, const CrabBuilderParams &params) {
    return isPointer(v.getType(), params);
  }

  /** Converts v to z_number. Assumes that v is signed */
  static z_number toZNumber(const llvm::APInt &v, const CrabBuilderParams &params,
			    bool& is_bignum) {
    is_bignum = false;
    if (!params.enable_bignums) {
      is_bignum = isSignedBigNum(v);
      
    }
    #if 0
    // Convert to strings is not ideal but it shouldn't be a big
    // bottleneck.
    std::string val = v.toString(10,true /*is signed*/);
    return z_number(val);
    #else
    // Based on:
    // https://llvm.org/svn/llvm-project/polly/trunk/lib/Support/GICHelper.cpp
    llvm::APInt abs;
    abs = v.isNegative() ? v.abs() : v;
    const uint64_t *rawdata = abs.getRawData();
    unsigned numWords = abs.getNumWords();
    
    ikos::z_number res;
    mpz_import(res.get_mpz_t(), numWords, -1,  sizeof(uint64_t), 0, 0, rawdata);
    return v.isNegative() ? -res : res;
    #endif 
  }

  // The return value should be z_number and not number_t
  static z_number getIntConstant(const ConstantInt* CI, const CrabBuilderParams &params,
				 bool& is_bignum){
    is_bignum = false;
    if (CI->getType()->isIntegerTy(1)) {
      return z_number((int64_t) CI->getZExtValue());
    } else {
      return toZNumber(CI->getValue(), params, is_bignum);
    }
  }

  static bool isTrackedType(const llvm::Type& ty, const CrabBuilderParams &params) {
    // -- a pointer
    if (ty.isPointerTy()) 
      return (params.track_pointers()); 
    
    // -- always track integer and boolean registers
    return ty.isIntegerTy();
  }

  static bool isTracked(const llvm::Value &v, const CrabBuilderParams &params) {
    // -- ignore any shadow variable created by seahorn
    if (v.getName().startswith("shadow.mem")) 
      return false;

    return isTrackedType(*v.getType(), params);
  }

  // A crab callsite should return a value if the I's callee has a
  // tracked return type, regardless whether the LLVM callsite
  // returns. In LLVM, a callsite does not need to fully match with
  // the function signature but in Crab we require to do so. E.g.,
  // LLVM can remove the return value of the callsite if it's dead.
  static bool ShouldCallSiteReturn(llvm::CallInst& I, const CrabBuilderParams &params) {
    llvm::CallSite CS(&I);
    if (llvm::Function* Callee = CS.getCalledFunction()) {
      Type* RT = Callee->getReturnType();
      return (!(RT->isVoidTy()) && isTrackedType(*RT, params));
    }
    return false;
  }

  // Whether the callsite returns a value.
  static bool DoesCallSiteReturn(llvm::CallInst& I, const CrabBuilderParams &params) {
    return (!I.getType()->isVoidTy() && isTracked(I, params));
  }

  
  // Convenient wrapper for a LLVM variable or constant
  class crabLit {
  public:
    enum lit_class_t {
      CRAB_LITERAL_BOOL,
      CRAB_LITERAL_INT,
      CRAB_LITERAL_PTR,
    };
    
    crabLit(lit_class_t lit_class) : m_lit_class(lit_class) {}

    virtual ~crabLit() {}

    bool isBool() const { return m_lit_class == CRAB_LITERAL_BOOL; }

    bool isInt() const { return m_lit_class == CRAB_LITERAL_INT; }

    bool isPtr() const { return m_lit_class == CRAB_LITERAL_PTR; }
        
    virtual bool isVar() const = 0;
    
    virtual var_t getVar() const = 0;

    virtual void write(crab::crab_os &out) const = 0;
    
  private:
    lit_class_t m_lit_class;
  };

  crab_os& operator<<(crab_os& out, const crabLit& l){
    l.write(out);
    return out;
  }
  
  /** A Boolean literal is either a variable or constants true and false. **/   
  class crabBoolLit: public crabLit {
    friend class crabLitFactoryImpl;
    
    bool m_cst; // only considered if m_var.is_null()
    var_ref_t m_var;
    
    crabBoolLit(bool cst): crabLit(CRAB_LITERAL_BOOL), m_cst(cst) {}
    
    crabBoolLit(var_t var): crabLit(CRAB_LITERAL_BOOL), m_var(var) {}
    
  public:
    
    bool isVar() const override {
      return (!m_var.is_null());
    }
    
    var_t getVar() const override {
      assert (isVar());
      return m_var.get();
    }
    
    bool isConst() const {
      return (m_var.is_null());
    }
    
    bool isTrue() const {
      if (!isConst()) return false;
      return m_cst;
    }
    
    bool isFalse() const {
      if (!isConst()) return false;
      return !m_cst; 
    }
    
    void write(crab::crab_os &out) const override {
      if (isVar()) {
	out << getVar();
      } else if (isTrue()) {
	out << "true";
      } else {
	out << "false";
      }
    }
  };
  
  /** A pointer literal is either a variable or constant null.**/
  class crabPtrLit: public crabLit {
    friend class crabLitFactoryImpl;
    
    var_ref_t m_lit; // if m_lit.is_null() then the literal represents null
    
    crabPtrLit()
      : crabLit(CRAB_LITERAL_PTR) {} // null
    crabPtrLit(var_t v)
      : crabLit(CRAB_LITERAL_PTR), m_lit(v) {}
    
  public:
    
    bool isVar() const override { return !m_lit.is_null(); }
    
    var_t getVar() const override {
      assert (isVar());
      return m_lit.get();
    }
    
    bool isNull() const { return m_lit.is_null(); }

    void write(crab::crab_os &out) const override {
      if (isVar()) {
	out << getVar();
      } else {
	out << "NULL";
      }
    }
  };
  
  /** A numerical literal is either a variable or constant number.**/
  class crabIntLit: public crabLit {
    friend class crabLitFactoryImpl;
    
    number_t m_num; // only considered if m_var.is_null();
    var_ref_t m_var;

    // If z_number != number_t we assume that number_t has a
    // constructor for z_number.
    explicit crabIntLit(z_number n): crabLit(CRAB_LITERAL_INT), m_num(n) {}
    
    explicit crabIntLit(var_t v): crabLit(CRAB_LITERAL_INT), m_var(v) {}
    
  public:
    
    bool isVar() const override { return !m_var.is_null(); }
    
    var_t getVar() const override{
      assert (isVar());
      return m_var.get();
    }
    
    bool isInt() const { return m_var.is_null(); }
    
    number_t getInt() const {
      assert (isInt());
      return m_num;
    }
    
    lin_exp_t getExp() const {
      if (isInt())
	return lin_exp_t(getInt());
      else {
	assert(isVar());
	return lin_exp_t(getVar());
      }
    }

    void write(crab::crab_os &out) const override {
      if (isVar()) {
	out << getVar();
      } else {
	out << getInt();
      }
    }
    
  };

  typedef std::shared_ptr<crabLit> crab_lit_ref_t;
  
  class crabLitFactoryImpl {
  public:
    
    crabLitFactoryImpl(llvm_variable_factory &vfac, const CrabBuilderParams& params);

    llvm_variable_factory& get_vfac() { return m_vfac; }

    const CrabBuilderParams& get_cfg_builder_params() const { return m_params;}

    // Translate v into a crab literal based on v's type
    crab_lit_ref_t getLit(const llvm::Value &v);

    // Create a fresh integer array variable
    var_t mkIntArrayVar(unsigned bitwidth);

    // Create a fresh boolean array variable    
    var_t mkBoolArrayVar();

    // Create a fresh pointer array variable        
    var_t mkPtrArrayVar();        
    
    // Create an array variable associated with region r.
    var_t mkArrayVar(mem_region_t r);

    // Create an scalar variable associated with region r.
    var_t mkArraySingletonVar(mem_region_t r);
    
    // Create a fresh integer variable of bitwidth bits
    var_t mkIntVar(unsigned bitwidth);

    // Create a fresh boolean variable
    var_t mkBoolVar();

    // Create a fresh pointer variable
    var_t mkPtrVar();

    // Create a fresh variable from a Value
    llvm::Optional<var_t> mkVar(const Value&v);
    
    // Common accessors to crab_lit_ref_t subclasses.
    bool isBoolTrue(const crab_lit_ref_t ref) const;
    
    bool isBoolFalse(const crab_lit_ref_t ref) const;

    bool isPtrNull(const crab_lit_ref_t ref) const;
    
    number_t getIntCst(const crab_lit_ref_t ref) const;

    lin_exp_t getExp(const crab_lit_ref_t ref) const;
        
  private:

    typedef std::unordered_map<const Value*, crab_lit_ref_t> lit_cache_t;
    typedef typename lit_cache_t::value_type binding_t;
    
    llvm_variable_factory &m_vfac;
    const CrabBuilderParams &m_params;
    lit_cache_t m_lit_cache;

    Optional<crabBoolLit> getBoolLit(const llvm::Value &v);
    Optional<crabIntLit> getIntLit(const llvm::Value &v);
    Optional<crabPtrLit> getPtrLit(const llvm::Value &v);    
  };
  
  crabLitFactoryImpl::crabLitFactoryImpl(llvm_variable_factory &vfac,
					 const CrabBuilderParams &params)
    : m_vfac(vfac), m_params(params) {}

  crab_lit_ref_t crabLitFactoryImpl::getLit(const llvm::Value &v) {
    auto it = m_lit_cache.find(&v);
    if (it != m_lit_cache.end()) {
      return it->second;
    }
    const llvm::Type &t = *v.getType();
    // Note that getBoolLit, getPtrLit and getIntLit are not aware of
    // which types are tracked or not. They only use type information
    // and not the track level.
    if (isBool(&t)) {
      Optional<crabBoolLit> lit = getBoolLit(v);
      if (lit.hasValue()) {
	crab_lit_ref_t ref =
	  std::static_pointer_cast<crabLit>(std::make_shared<crabBoolLit>(lit.getValue()));
	m_lit_cache.insert(binding_t(&v, ref));
	return ref;	  
      }
    } else if (isInteger(&t)) {
      Optional<crabIntLit> lit = getIntLit(v);
      if (lit.hasValue()) {
	crab_lit_ref_t ref =
	  std::static_pointer_cast<crabLit>(std::make_shared<crabIntLit>(lit.getValue()));
	m_lit_cache.insert(binding_t(&v, ref));
	return ref;
      }
    } else if (t.isPointerTy()) {
      Optional<crabPtrLit> lit = getPtrLit(v);
      if (lit.hasValue()) {
	crab_lit_ref_t ref =
	  std::static_pointer_cast<crabLit>(std::make_shared<crabPtrLit>(lit.getValue()));
	m_lit_cache.insert(binding_t(&v, ref));
	return ref;
      }
    }
    return nullptr;
  }
  
  var_t crabLitFactoryImpl::mkArrayVar(mem_region_t mem_region) {
    crab::variable_type type = crab::UNK_TYPE;
    unsigned bitwidth = 0; /* unknown */
    switch  (mem_region.get_type()) {
    case INT_REGION :
      type = ARR_INT_TYPE;
      bitwidth = mem_region.get_bitwidth();
      break;
    case BOOL_REGION:
      type = ARR_BOOL_TYPE;
      bitwidth = 1;
      break;
    case PTR_REGION :
      type = ARR_PTR_TYPE;
      break;
    default: CLAM_ERROR("unsupported region type");
    }
    return var_t(m_vfac.get(mem_region.get_id()), type, bitwidth);
  }

  var_t crabLitFactoryImpl::mkArraySingletonVar(mem_region_t mem_region) {
    crab::variable_type type = crab::UNK_TYPE;
    unsigned bitwidth = 0; /* unknown */
    if (const Value* v= mem_region.getSingleton()) {
      Type* ty = cast<PointerType>(v->getType())->getElementType();      
      bitwidth = ty->getIntegerBitWidth();
      if (mem_region.get_type() == INT_REGION && bitwidth <= 1) {
	CLAM_ERROR("Integer region must have bitwidth > 1");
      }
      // If the singleton contains a pointer then getIntegerBitWidth()
      // returns zero which means for us "unknown" bitwidth so we are
      // good.
    } else {
      CLAM_ERROR("Memory region does not belong to a global singleton");
    }
    switch  (mem_region.get_type()) {
    case INT_REGION : type = INT_TYPE; break;
    case BOOL_REGION: type = BOOL_TYPE; break;
    case PTR_REGION : type = PTR_TYPE; break;
    default: CLAM_ERROR("unsupported region type");
    }
    return var_t(m_vfac.get(mem_region.get_id()), type, bitwidth);
  }

  var_t crabLitFactoryImpl::mkIntArrayVar(unsigned bitwidth)
  { return var_t(m_vfac.get(), ARR_INT_TYPE, bitwidth); }     
  
  var_t crabLitFactoryImpl::mkBoolArrayVar()
  { return var_t(m_vfac.get(), ARR_BOOL_TYPE, 1); }
  
  var_t crabLitFactoryImpl::mkPtrArrayVar() 
  {  return var_t(m_vfac.get(), ARR_PTR_TYPE); }     
  
  var_t crabLitFactoryImpl::mkIntVar(unsigned bitwidth)
  { return var_t(m_vfac.get(), INT_TYPE, bitwidth);}     
  
  var_t crabLitFactoryImpl::mkBoolVar()
  { return var_t(m_vfac.get(), BOOL_TYPE, 1); }
  
  var_t crabLitFactoryImpl::mkPtrVar() 
  {  return var_t(m_vfac.get(), PTR_TYPE); }     

  llvm::Optional<var_t> crabLitFactoryImpl::mkVar(const Value&v)  {
    if (isBool(v)) {
      return mkBoolVar();
    } else if (isInteger(v)) {
      unsigned bitwidth = v.getType()->getIntegerBitWidth();      
      return mkIntVar(bitwidth);
    } else if (isPointer(v, m_params)) {
      return mkPtrVar();      
    }
    return llvm::None;
  }
  
  bool crabLitFactoryImpl::isBoolTrue(const crab_lit_ref_t ref) const {
    if (!ref || !ref->isBool())
      CLAM_ERROR("Literal is not a Boolean");
    auto lit = std::static_pointer_cast<const crabBoolLit>(ref);
    return lit->isTrue();
  }
    
  bool crabLitFactoryImpl::isBoolFalse(const crab_lit_ref_t ref) const {
    if (!ref || !ref->isBool())
      CLAM_ERROR("Literal is not a Boolean");    
    auto lit = std::static_pointer_cast<const crabBoolLit>(ref);
    return lit->isFalse();
  }
  
  bool crabLitFactoryImpl::isPtrNull(const crab_lit_ref_t ref) const {
    if (!ref || !ref->isPtr())
      CLAM_ERROR("Literal is not a pointer");        
    auto lit = std::static_pointer_cast<const crabPtrLit>(ref);
    return lit->isNull();
  }
  
  lin_exp_t crabLitFactoryImpl::getExp(const crab_lit_ref_t ref) const {
    if (!ref || !ref->isInt())
      CLAM_ERROR("Literal is not an integer");            
    auto lit = std::static_pointer_cast<const crabIntLit>(ref);
    return lit->getExp();
  }

  number_t crabLitFactoryImpl::getIntCst(const crab_lit_ref_t ref) const {
    if (!ref || !ref->isInt())
      CLAM_ERROR("Literal is not an integer");                
    auto lit = std::static_pointer_cast<const crabIntLit>(ref);
    return lit->getInt();
  }
  
  Optional<crabBoolLit> crabLitFactoryImpl::getBoolLit(const llvm::Value &v){
    if (isBool(v)) {
      if (const llvm::ConstantInt *c = llvm::dyn_cast<const llvm::ConstantInt>(&v)) {
	// -- constant boolean
	bool is_bignum;
	z_number n = getIntConstant(c, m_params, is_bignum);
	if (!is_bignum) {
	  return crabBoolLit(n > 0 ? true : false);
	}
      } else if (!llvm::isa<llvm::ConstantExpr>(v)) {
	// -- boolean variable 
	return crabBoolLit(var_t(m_vfac[&v], BOOL_TYPE, 1));
      }
    }
    return llvm::None;
  }
  
  Optional<crabPtrLit> crabLitFactoryImpl::getPtrLit(const llvm::Value &v){
    if (llvm::isa<llvm::ConstantPointerNull>(&v)) {
      // -- constant null
      return crabPtrLit();
    } else if (v.getType()->isPointerTy() && !llvm::isa<llvm::ConstantExpr>(v)) {
      // -- pointer variable 
      return crabPtrLit(var_t(m_vfac[&v], PTR_TYPE));
    }
    return llvm::None;
  }
  
  Optional<crabIntLit> crabLitFactoryImpl::getIntLit(const llvm::Value &v){
    if (isInteger(v)) {
      if (const llvm::ConstantInt *c = llvm::dyn_cast<const llvm::ConstantInt>(&v)) {
	// -- constant integer
	bool is_bignum;	
	z_number n = getIntConstant(c, m_params, is_bignum);
	if (!is_bignum) {
	  return crabIntLit(n);
	}
      } else if (!llvm::isa<llvm::ConstantExpr>(v)) {
	// -- integer variable
	unsigned bitwidth = v.getType()->getIntegerBitWidth();
	return crabIntLit(var_t(m_vfac[&v], INT_TYPE, bitwidth));
      }
    }
    return llvm::None;
  }

  
  /** 
      Factory to create crab literals: typed variable or number.
  **/
  class crabLitFactory {
  public:
    
    crabLitFactory(llvm_variable_factory &vfac, const CrabBuilderParams& params);

    ~crabLitFactory();
    
    llvm_variable_factory& get_vfac();

    crab::cfg::tracked_precision get_track() const;
    
    const CrabBuilderParams& get_cfg_builder_params() const;

    /** convert a Value to a crabLit **/
    crab_lit_ref_t getLit(const llvm::Value &v);

    /** make typed variables **/
    var_t mkIntVar(unsigned bitwidth);
    
    var_t mkBoolVar();
    
    var_t mkPtrVar();

    llvm::Optional<var_t> mkVar(const llvm::Value&v);
    
    var_t mkIntArrayVar(unsigned bitwidth);
    
    var_t mkBoolArrayVar();
    
    var_t mkPtrArrayVar();
    
    template<typename Region> var_t mkArrayVar(Region r);
    
    template<typename Region> var_t mkArraySingletonVar(Region r);    

    /** direct accessors to crabLit subclasses **/
    bool isBoolTrue(const crab_lit_ref_t ref) const;
    
    bool isBoolFalse(const crab_lit_ref_t ref) const;

    bool isPtrNull(const crab_lit_ref_t ref) const;

    lin_exp_t getExp(const crab_lit_ref_t ref) const;
    
    number_t getIntCst(const crab_lit_ref_t ref) const;    
          
  private:
    crabLitFactoryImpl* m_impl;
  };

  struct pair_hash {
    template<typename T1, typename T2>
    std::size_t operator()(const std::pair<T1,T2> &p) const {
      std::size_t seed = 0;
      boost::hash_combine(seed, p.first);
      boost::hash_combine(seed, p.second);	
      return seed;
    }
  };
    
  crabLitFactory::crabLitFactory(llvm_variable_factory &vfac, const CrabBuilderParams &params)
    : m_impl(new crabLitFactoryImpl(vfac, params)) { }
    
  crabLitFactory::~crabLitFactory() {
    delete m_impl;
  }

  llvm_variable_factory& crabLitFactory::get_vfac()
  { return m_impl->get_vfac(); }

  crab::cfg::tracked_precision crabLitFactory::get_track() const {
    return get_cfg_builder_params().precision_level;
  }

  const CrabBuilderParams& crabLitFactory::get_cfg_builder_params() const {
    return m_impl->get_cfg_builder_params();
  }

  crab_lit_ref_t crabLitFactory::getLit(const llvm::Value &v)
  { return m_impl->getLit(v);}

  var_t crabLitFactory::mkIntArrayVar(unsigned bitwidth)
  { return m_impl->mkIntArrayVar(bitwidth);}
    
  var_t crabLitFactory::mkBoolArrayVar()
  { return m_impl->mkBoolArrayVar();}    
    
  var_t crabLitFactory::mkPtrArrayVar()
  { return m_impl->mkPtrArrayVar();}        
  
  template<>
  var_t crabLitFactory::mkArrayVar(mem_region_t r)
  { return m_impl->mkArrayVar(r);}

  template<>
  var_t crabLitFactory::mkArraySingletonVar(mem_region_t r)
  { return m_impl->mkArraySingletonVar(r);}
  
  var_t crabLitFactory::mkIntVar(unsigned bitwidth)
  { return m_impl->mkIntVar(bitwidth);}
    
  var_t crabLitFactory::mkBoolVar()
  { return m_impl->mkBoolVar();}    
    
  var_t crabLitFactory::mkPtrVar()
  { return m_impl->mkPtrVar();}        

  llvm::Optional<var_t> crabLitFactory::mkVar(const Value& v)
  { return m_impl->mkVar(v);}        
    
  bool crabLitFactory::isBoolTrue(const crab_lit_ref_t ref) const
  { return m_impl->isBoolTrue(ref);}
    
  bool crabLitFactory::isBoolFalse(const crab_lit_ref_t ref) const
  { return m_impl->isBoolFalse(ref);}    
  
  bool crabLitFactory::isPtrNull(const crab_lit_ref_t ref) const
  { return m_impl->isPtrNull(ref);}
  
  lin_exp_t crabLitFactory::getExp(const crab_lit_ref_t ref) const
  { return m_impl->getExp(ref);}

  number_t crabLitFactory::getIntCst(const crab_lit_ref_t ref) const
  { return m_impl->getIntCst(ref);}


  /* 
     Helpers for memory regions.

     We don't add array statements for memory regions containing
     pointers. This means that if the load's lhs or store value
     operand is a pointer we only add the corresponding pointer
     statement (ptr_load/ptr_store) but not any extra array statement
     (array_load/array_store). 

     FIXME: If we would want to add array statements with elements of
     pointer type, we need to do some renaming. Otherwise, for
     instance, for the lhs of a load instruction, the same variable
     name would be used both for ptr_load and array_load with
     contradicting types.
  */
  
  static inline mem_region_t
  get_region(HeapAbstraction &mem, Function& f, Value*v) {
    mem_region_t res = mem.getRegion(f, v);
    if (res.get_type() == INT_REGION || res.get_type() == BOOL_REGION) {
      return res;
    } else {
      return mem_region_t();
    }
  }

  template<typename V>
  static inline mem_region_vector_t get_read_only_regions(HeapAbstraction &mem, V& v) {
    mem_region_vector_t res;
    auto regions = mem.getOnlyReadRegions(v);
    std::copy_if(regions.begin(), regions.end(), std::back_inserter(res),
		 [](mem_region_t r){
		   return r.get_type() == INT_REGION || r.get_type() == BOOL_REGION;
		 });
    return res;
  }

  template<typename V>
  static inline mem_region_vector_t get_modified_regions(HeapAbstraction &mem, V& v) {
    mem_region_vector_t res;
    auto regions = mem.getModifiedRegions(v);
    std::copy_if(regions.begin(), regions.end(), std::back_inserter(res),
		 [](mem_region_t r){
		   return r.get_type() == INT_REGION || r.get_type() == BOOL_REGION;
		 });
    return res;
  }

  template<typename V>
  static inline mem_region_vector_t get_new_regions(HeapAbstraction &mem, V& v) {
    mem_region_vector_t res;
    auto regions = mem.getNewRegions(v);
    std::copy_if(regions.begin(), regions.end(), std::back_inserter(res),
		 [](mem_region_t r){
		   return r.get_type() == INT_REGION || r.get_type() == BOOL_REGION;
		 });
    return res;
  }


  static bool hasDebugLoc(const Instruction *inst) {
    if (!inst) return false;
    const DebugLoc &dloc = inst->getDebugLoc();
    return dloc;
  }

  static crab::cfg::debug_info getDebugLoc(const Instruction *inst) {
    if (!hasDebugLoc(inst))
      return crab::cfg::debug_info();
    const DebugLoc &dloc = inst->getDebugLoc();
    unsigned Line = dloc.getLine();
    unsigned Col = dloc.getCol();
    std::string File = (*dloc).getFilename();
    if (File == "")
      File = "unknown file";
    return crab::cfg::debug_info(File, Line, Col);
  }

  static uint64_t storageSize(const Type *t, const DataLayout &dl) {
    return dl.getTypeStoreSize(const_cast<Type*>(t));
  }
  
  static void normalizeCmpInst(CmpInst &I) {
    switch (I.getPredicate()){
      case ICmpInst::ICMP_UGT:	
      case ICmpInst::ICMP_SGT: I.swapOperands(); break; 
      case ICmpInst::ICMP_UGE:	
      case ICmpInst::ICMP_SGE: I.swapOperands(); break; 
      default: ;
    }
  }

  static bool isIntCast(const CastInst &I) {
    return I.isIntegerCast();
  }

  static bool isPointerCast(const CastInst &I) {
    return isa<IntToPtrInst>(I) || isa<PtrToIntInst>(I) || isa<BitCastInst>(I);
  }
  
  static bool isIntToBool(const CastInst &I) {
    return (isa<TruncInst>(I) && I.getDestTy()->isIntegerTy(1));
  }

  static bool isBoolToInt(const CastInst &I) {
    return ((isa<ZExtInst>(I) || isa<SExtInst>(I))  && I.getSrcTy()->isIntegerTy(1));
  }

  static bool isBoolArray(const Type &T) {
    return (T.isArrayTy() && T.getArrayElementType()->isIntegerTy(1));
  }
  
  static bool isIntArray(const Type &T) {
    return (T.isArrayTy() &&
	    T.getArrayElementType()->isIntegerTy() &&
	    !(T.getArrayElementType()->isIntegerTy(1)));
  }

  // static bool isPointerArray(const Type &T) {
  //   return (T.isArrayTy() && T.getArrayElementType()->isPointerTy());
  // }
  
  static bool isAssertFn(const Function* F) {
    return (F->getName().equals("verifier.assert") || 
            F->getName().equals("crab.assert") || 
            F->getName().equals("__CRAB_assert"));
  }


  static bool isSeaHornFail(const Function* F) {
    return (F->getName().equals("seahorn.fail"));
  }
  
  static bool isErrorFn(const Function* F) {
    return (F->getName().equals("seahorn.error") ||
	    F->getName().equals("verifier.error") ||
	    F->getName().equals("__VERIFIER_error") || 	    	    
            F->getName().equals("__SEAHORN_error"));
  }

  static bool isAssumeFn(const Function* F) {
    return (F->getName().equals("verifier.assume") ||
	    F->getName().equals("__VERIFIER_assume") ||
	    F->getName().equals("__CRAB_assume"));
  }

  static bool isNotAssumeFn(const Function* F) {
    return (F->getName().equals("verifier.assume.not") ||
	    F->getName().equals("__VERIFIER_assume_not") ||
	    F->getName().equals("__CRAB_assume_not"));
  }

  static bool isVerifierCall(const Function *F) {
    return (isAssertFn(F) || isErrorFn(F) ||
	    isAssumeFn(F) || isNotAssumeFn(F) ||
	    isSeaHornFail(F));
  }

  static bool isZeroInitializer(const Function *F) {
    return F->getName().startswith("verifier.zero_initializer");
  }

  static bool isIntInitializer(const Function *F) {
    return F->getName().startswith("verifier.int_initializer");
  }
  
  // Return true if all uses are BranchInst's
  static bool AllUsesAreBrInst(Value* V) {
    // XXX: do not strip pointers here
    for (auto &U: V->uses())
      if (!isa<BranchInst>(U.getUser()))
  	return false;
    return true;
  }  

  // Return true if all uses are BranchInst's or Select's
  static bool AllUsesAreBrOrIntSelectCondInst(Value* V) {
    // XXX: do not strip pointers here
    for (auto &U: V->uses()) {
      if ((!isa<BranchInst>(U.getUser())) &&(!isa<SelectInst>(U.getUser())))
  	return false;
      if (SelectInst *SI = dyn_cast<SelectInst>(U.getUser())) {
	if (isBool(*SI) || SI->getCondition() != V) {
	  // if the operands are bool or V is not the condition
	  return false;
	}
      }
    }
    return true;
  }
  
  // Return true if all uses are the callee at callsites
  static bool AllUsesAreIndirectCalls(Value* V)  {
    // XXX: do not strip pointers here
    for (auto &U: V->uses()) {
      if (CallInst *CI = dyn_cast<CallInst>(U.getUser())) {
	CallSite CS(CI);
	const Value *callee = CS.getCalledValue();
	if (callee == V) continue;
      }
      return false;
    }
    return true;
  }

  // Return true if all uses are verifier calls (assume/assert)
  static bool AllUsesAreVerifierCalls(Value* V)  {
    for (auto &U: V->uses()) {
      if (CallInst *CI = dyn_cast<CallInst>(U.getUser())) {
	CallSite CS(CI);
	const Value *calleeV = CS.getCalledValue();
	const Function *callee = dyn_cast<Function>(calleeV->stripPointerCasts());
	if (callee && 
	    (isAssertFn(callee) || isAssumeFn(callee) || isNotAssumeFn(callee))) {
	  continue;
	}
      }
      return false;
    }
    return true;
  }
  
  
  // Return true if all uses are GEPs
  static bool AllUsesAreGEP(Value *V) {
    for (auto &U: V->uses())
      if (!isa<GetElementPtrInst>(U.getUser()))
	return false;
    return true;
  }

  // Return whether the region contains a singleton alias class
  template<typename HeapAbstraction>
  static const Value* isGlobalSingleton(Region<HeapAbstraction> r,
					bool enable_unique_scalars) {
    if (enable_unique_scalars) {
      if (r.isUnknown()) return nullptr;
      if (r.get_type() == INT_REGION || r.get_type() == BOOL_REGION) {
	if (const Value* v = r.getSingleton()) {
	  return v;
	}
      }
    }
    return nullptr;
  }

  static void havoc(var_t v, basic_block_t &bb, bool include_useless_havoc) {
    if (include_useless_havoc) {
      bb.havoc(v);
    }
  }
    
  // %x = icmp geq %y, 10  ---> bool_assign(%x, y >= 0)
  static void cmpInstToCrabBool(CmpInst &I, crabLitFactory &lfac, basic_block_t &bb) {
    // The type of I is a boolean or vector of booleans
    normalizeCmpInst(I);
    
    const Value& v0 = *I.getOperand(0);
    const Value& v1 = *I.getOperand(1);

    crab_lit_ref_t ref = lfac.getLit(I);
    if (!ref || !(ref->isBool()) || !(ref->isVar())) {
      // It could be here if the type of I is a vector of booleans.
      // We prefer to raise an error.
      CLAM_ERROR("lhs of CmpInst should be a Boolean");
    }
    var_t lhs = ref->getVar();

    crab_lit_ref_t ref0 = lfac.getLit(v0);
    if (!ref0 || !(ref0->isInt())) {
      havoc(lhs, bb, lfac.get_cfg_builder_params().include_useless_havoc);
      return;
    }

    crab_lit_ref_t ref1 = lfac.getLit(v1);
    if (!ref1 || !(ref1->isInt())) {
      havoc(lhs, bb, lfac.get_cfg_builder_params().include_useless_havoc);
      return;
    }
    
    lin_exp_t op0 = lfac.getExp(ref0);
    lin_exp_t op1 = lfac.getExp(ref1);

    assert(isBool(I));
    switch (I.getPredicate()) {
    case CmpInst::ICMP_EQ: {
      lin_cst_t cst(op0 == op1);
      bb.bool_assign(lhs, cst);
      break;
    }
    case CmpInst::ICMP_NE: {
      lin_cst_t cst(op0 != op1);
      bb.bool_assign(lhs, cst);
      break;
    }
    case CmpInst::ICMP_ULT: 
    case CmpInst::ICMP_SLT: {
      lin_cst_t cst(op0 <= op1 - number_t(1));
      if (I.getPredicate() == CmpInst::ICMP_ULT) {
	cst.set_unsigned();
      }
      bb.bool_assign(lhs, cst);
      break;
    }
    case CmpInst::ICMP_ULE:
    case CmpInst::ICMP_SLE: {
      lin_cst_t cst(op0 <= op1);
      if (I.getPredicate() == CmpInst::ICMP_ULE) {
	cst.set_unsigned();
      }
      bb.bool_assign(lhs, cst);
      break;
    }	
    default:  
      CLAM_ERROR("unexpected problem while translating CmpInst");
    }
  }

  /* If possible, return a pointer constraint from CmpInst */  
  static Optional<ptr_cst_t>
  cmpInstToCrabPtr(CmpInst &I, crabLitFactory &lfac, const bool isNegated) {
    normalizeCmpInst(I);
    
    const Value& v0 = *I.getOperand(0);
    const Value& v1 = *I.getOperand(1);

    crab_lit_ref_t ref0 = lfac.getLit(v0);
    if (!ref0 || !(ref0->isPtr()))
      return llvm::None;

    crab_lit_ref_t ref1 = lfac.getLit(v1);
    if (!ref1 || !(ref1->isPtr()))
      return llvm::None;

    if (I.getPredicate() != CmpInst::ICMP_EQ &&
	I.getPredicate() != CmpInst::ICMP_NE) {
      //CLAM_WARNING("unexpected pointer comparison " << I);
      return llvm::None;            
    }
    
    bool is_eq;
    if ((I.getPredicate() == CmpInst::ICMP_EQ && !isNegated) ||
	(I.getPredicate() == CmpInst::ICMP_NE && isNegated)) {
      is_eq = true;
    } else {
      is_eq = false;
    }
    
    if (is_eq) {
      if (ref0->isVar() && lfac.isPtrNull(ref1)) {
	return ptr_cst_t::mk_eq_null(ref0->getVar());
      } else if (lfac.isPtrNull(ref0) && ref1->isVar()) {
	return ptr_cst_t::mk_eq_null(ref1->getVar());
      } else if (ref0->isVar() && ref1->isVar()) {
	return ptr_cst_t::mk_eq(ref0->getVar(), ref1->getVar());
      } else {
	return ptr_cst_t::mk_true();
      }
    } else {
      if (ref0->isVar() && lfac.isPtrNull(ref1)) {
	return ptr_cst_t::mk_diseq_null(ref0->getVar());
      } else if (lfac.isPtrNull(ref0) && ref1->isVar()) {
	return ptr_cst_t::mk_diseq_null(ref1->getVar());
      } else if (ref0->isVar() && ref1->isVar()) {
	return ptr_cst_t::mk_diseq(ref0->getVar(), ref1->getVar());
      } else {
	return ptr_cst_t::mk_false();
      }      
    }
  }

  /* If possible, return a linear constraint from CmpInst */
  static Optional<lin_cst_t>
  cmpInstToCrabInt(CmpInst &I, crabLitFactory &lfac, const bool isNegated = false) {
    normalizeCmpInst(I);
    
    const Value& v0 = *I.getOperand(0);
    const Value& v1 = *I.getOperand(1);

    crab_lit_ref_t ref0 = lfac.getLit(v0);
    if (!ref0 || !(ref0->isInt()))
      return llvm::None; 

    crab_lit_ref_t ref1 = lfac.getLit(v1);
    if (!ref1 || !(ref1->isInt()))
      return llvm::None; 
    
    lin_exp_t op0 = lfac.getExp(ref0);
    lin_exp_t op1 = lfac.getExp(ref1);
    
    switch (I.getPredicate()) {
    case CmpInst::ICMP_EQ:
      if (!isNegated)
	return lin_cst_t(op0 == op1);
      else
	return lin_cst_t(op0 != op1);
      break;
    case CmpInst::ICMP_NE:
      if (!isNegated)
	return lin_cst_t(op0 != op1);
      else
	return lin_cst_t(op0 == op1);
	break;
    case CmpInst::ICMP_ULT: 
    case CmpInst::ICMP_SLT: {
      lin_cst_t cst;
      if (!isNegated)
	cst = lin_cst_t(op0 <= op1 - number_t(1));
      else
	cst = lin_cst_t(op0 >= op1);
      if (I.getPredicate() == CmpInst::ICMP_ULT) {
	cst.set_unsigned();
      }
      return cst;
      break;
    }
    case CmpInst::ICMP_ULE:
    case CmpInst::ICMP_SLE: {
      lin_cst_t cst;
      if (!isNegated)
	cst = lin_cst_t(op0 <= op1);
      else
	cst = lin_cst_t(op0 >= op1 + number_t(1));
      if (I.getPredicate() == CmpInst::ICMP_ULE) {
	cst.set_unsigned();
      }
      return cst;
      break;
    }
    default: ;;  
    }
    return llvm::None; 
  }

  // This function makes sure that all actual parameters and function
  // return values are variables. This is required by crab.
  // precondition: v is tracked.
  static var_t normalizeFuncParamOrRet(Value& v, basic_block_t &bb, crabLitFactory &lfac) {
    if (crab_lit_ref_t ref = lfac.getLit(v)) {
      if (ref->isVar()) {
	return ref->getVar();
      } else {
	// must be constant
	if (ref->isInt()) {
	  unsigned bitwidth = v.getType()->getIntegerBitWidth();      
	  var_t res = lfac.mkIntVar(bitwidth);
	  bb.assign(res, lfac.getExp(ref));
	  return res;
	} else if (ref->isBool()) {
	  var_t res = lfac.mkBoolVar();
	  bb.bool_assign(res, lfac.isBoolTrue(ref) ?
   			 lin_cst_t::get_true(): lin_cst_t::get_false());
	  return res;
	} else if (ref->isPtr()) {
	  var_t res = lfac.mkPtrVar();
	  bb.ptr_null(res);
	  return res;
	}
      }
    }

    if (isTracked(v, lfac.get_cfg_builder_params())) {
      if (isa<ConstantExpr>(v)) {
	CLAM_WARNING("Clam cfg builder created a fresh variable from constant expr");
	llvm::Optional<var_t> fresh_v = lfac.mkVar(v);
	if (fresh_v.hasValue()) {
	  return fresh_v.getValue();
	}
      }
      // we should not reach this point since v is tracked.
      CLAM_ERROR("cannot normalize function parameter or return value");
      // clang complains otherwise
      abort();
    }
  }
  
  //! Translate PHI nodes
  struct CrabPhiVisitor : public InstVisitor<CrabPhiVisitor> {
    
    crabLitFactory &m_lfac;
    HeapAbstraction& m_mem;
    // block where assignment will be inserted
    basic_block_t& m_bb; 
    // incoming block of the PHI instruction
    const BasicBlock& m_inc_BB; 

    CrabPhiVisitor(crabLitFactory &lfac,
		    HeapAbstraction& mem,
		    basic_block_t& bb, 
                    const BasicBlock& inc_BB): 
      m_lfac(lfac), m_mem(mem), m_bb(bb), m_inc_BB(inc_BB) {}
    
    void visitBasicBlock(BasicBlock &BB) {
      auto curr = BB.begin();
      if (!isa<PHINode>(curr)) return;

      DenseMap<const Value*, var_t> old_val_map;

      // --- All the phi-nodes must be evaluated atomically. This
      //     means that if one phi node v1 has as incoming value
      //     another phi node v2 in the same block then it should take
      //     the v2's old value (i.e., before v2's evaluation).

      for (; PHINode *phi = dyn_cast<PHINode>(curr); ++curr) {        
        const Value &v = *phi->getIncomingValueForBlock(&m_inc_BB);
        if (!isTracked(v, m_lfac.get_cfg_builder_params())) continue;
        const PHINode* phi_v = dyn_cast<PHINode>(&v);
        if (phi_v && (phi_v->getParent() == &BB)) {
	  // -- save the old version of the variable that maps to the
	  //    phi node v
	  auto it = old_val_map.find(&v);
	  if (it == old_val_map.end()) {
	    if (crab_lit_ref_t phi_val_ref = m_lfac.getLit(v)) {
	      if (phi_val_ref->isBool()) {
		var_t lhs = m_lfac.mkBoolVar();
		if (phi_val_ref->isVar()) {
		  m_bb.bool_assign(lhs, phi_val_ref->getVar());
		} else {
		  m_bb.bool_assign(lhs, m_lfac.isBoolTrue(phi_val_ref) ?
				   lin_cst_t::get_true() : lin_cst_t::get_false());		
		}		
	    	old_val_map.insert(std::make_pair(&v, lhs));
	      } else if (phi_val_ref->isInt()) {
		var_t lhs = m_lfac.mkIntVar(phi_v->getType()->getIntegerBitWidth());
		m_bb.assign(lhs, m_lfac.getExp(phi_val_ref));
	    	old_val_map.insert(std::make_pair(&v, lhs));		
	      } else if (phi_val_ref->isPtr()) {
		var_t lhs = m_lfac.mkPtrVar();
		if (phi_val_ref->isVar()) {
		  m_bb.ptr_assign(lhs, phi_val_ref->getVar(), number_t(0));		  	
		} else {
		  m_bb.ptr_null(lhs);
		}
	    	old_val_map.insert(std::make_pair(&v, lhs));		
	      }
	    } else {
	      CLAM_ERROR("unexpected PHI node");
	    }
	  }
	}
      }
      
      curr = BB.begin();
      for (; isa<PHINode>(curr); ++curr) {
        PHINode &phi = *cast<PHINode>(curr);
        if (!isTracked(phi, m_lfac.get_cfg_builder_params())) continue;	
        const Value &v = *phi.getIncomingValueForBlock(&m_inc_BB);

	crab_lit_ref_t lhs_ref = m_lfac.getLit(phi);
	if (!lhs_ref || !lhs_ref->isVar()) {
	  CLAM_ERROR("unexpected PHI instruction");
	} 
	var_t lhs = lhs_ref->getVar();
        auto it = old_val_map.find(&v);
        if (it != old_val_map.end()) {
	  // -- use old version if exists
	  if (isBool(phi)) {
	    m_bb.bool_assign(lhs, it->second);
	  } else if (phi.getType()->isIntegerTy()) {
	    m_bb.assign(lhs, it->second);
	  } else if (isPointer(phi, m_lfac.get_cfg_builder_params())){
	    m_bb.ptr_assign(lhs, it->second, number_t(0));
	  }
        } else {
	  if (crab_lit_ref_t phi_val_ref = m_lfac.getLit(v)) {
	    if (phi_val_ref->isBool()) {
	      if (phi_val_ref->isVar()) {
		m_bb.bool_assign(lhs, phi_val_ref->getVar());
	      } else {
		m_bb.bool_assign(lhs, m_lfac.isBoolTrue(phi_val_ref) ?
				 lin_cst_t::get_true() : lin_cst_t::get_false());		
	      }
	    } else if (phi_val_ref->isInt()) {
	      m_bb.assign(lhs, m_lfac.getExp(phi_val_ref));
	    } else if (phi_val_ref->isPtr()) {
	      if (phi_val_ref->isVar()) {
		m_bb.ptr_assign(lhs, phi_val_ref->getVar(), number_t(0));		  	
	      } else {
		m_bb.ptr_null(lhs);
	      }
	    } else { /* unreachable*/ }
	  } else {
	    // we can be here if the incoming value is a bignum and we
	    // don't allow bignums.
	    m_bb.havoc(lhs);
	  }
	}
      }
    }
  };
			     
  //! Translate the rest of instructions
  class CrabInstVisitor : public InstVisitor<CrabInstVisitor> {

    crabLitFactory& m_lfac;
    HeapAbstraction& m_mem;
    const DataLayout* m_dl;
    const TargetLibraryInfo* m_tli;
    basic_block_t& m_bb;
    unsigned int m_object_id;
    bool m_has_seahorn_fail;
    // reverse **partial** map from Crab statements to LLVM instructions
    DenseMap<const statement_t*,const Instruction*> &m_rev_map;
    // to initialize arrays 
    std::set<mem_region_t> &m_init_regions;
    const CrabBuilderParams &m_params;
        
    unsigned fieldOffset(const StructType *t, unsigned field) const {
      return m_dl->getStructLayout(const_cast<StructType*>(t))->
          getElementOffset(field);
    }

    uint64_t storageSize(const Type *t) const {
      return clam::storageSize(t, *m_dl);
    }

    /* 
     *  Special function to return an unconstrained array index
     *  variable. This is used when we cannot statically know the integeroffset
     *  of a pointer with respect to its memory object.
     */
    var_t get_unconstrained_array_index_variable(llvm_variable_factory &vfac) {
      // use static to return always the same variable to save id's
      /*static*/
      var_t v(vfac.get(), crab::INT_TYPE, 32);
      // XXX: we dont' need to havoc'ed since this variable is never constrained.
      //m_bb.havoc(v);
      return v;
    }

    /* 
     *  Insert key-value in the reverse map but only if no CFG
     *  simplifications enabled 
     */
    void insert_rev_map(const statement_t* s, Instruction& inst) {
      if (!m_params.simplify) {
	m_rev_map.insert({s, &inst});
      }
    }
        
    // Return true if all uses of V are non-trackable memory accesses.
    // Useful to avoid translating bitcode that won't have any effect
    // anyway.
    bool AllUsesAreNonTrackMem(Value* V) const {
      // XXX: not sure if we should strip pointers here
      V = V->stripPointerCasts();
      for (auto &U: V->uses()) {
        if (StoreInst *SI = dyn_cast<StoreInst>(U.getUser())) {
	  if (Instruction *I = dyn_cast<Instruction>(V))  {
	    Function& parent = *(I->getParent()->getParent());	    
	    if (get_region(m_mem,parent,SI->getPointerOperand()).isUnknown() &&
		(!SI->getValueOperand()->getType()->isPointerTy() ||
		 get_region(m_mem,parent,SI->getValueOperand()).isUnknown()))
	      continue;
	  }
	  return false;
        }
        else if (LoadInst *LI = dyn_cast<LoadInst>(U.getUser())) {
	  if (Instruction *I = dyn_cast<Instruction>(V))  {
	    Function& parent = *(I->getParent()->getParent());	    
	    if (get_region(m_mem,parent,LI->getPointerOperand()).isUnknown() &&
		(!I->getType()->isPointerTy() ||
		 get_region(m_mem, parent, LI).isUnknown()))
	      continue;
	  }
	  return false;
        }
        else if (CallInst *CI = dyn_cast<CallInst>(U.getUser())) { 
          CallSite CS(CI);
          Function* callee = CS.getCalledFunction();
          if (callee && (callee->getName().startswith("llvm.dbg") || 
                         callee->getName().startswith("shadow.mem")))
            continue;
          else // conservatively return false
            return false; 
        }
        else
          return false;
      }
      return true;
    }
    
    void doBinOp(unsigned op, var_t lhs, lin_exp_t op1, lin_exp_t op2) {
      switch(op) {
      case BinaryOperator::Add:
	if (op1.get_variable() && op2.get_variable())	  
	  m_bb.add(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.add(lhs, (*op1.get_variable()), op2.constant());	    
	return;
      case BinaryOperator::Sub:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.sub(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.sub(lhs, (*op1.get_variable()), op2.constant());	    
	return;
      case BinaryOperator::Mul:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.mul(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.mul(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::SDiv:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.div(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.div(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::UDiv:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.udiv(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.udiv(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::SRem:
	if (op1.get_variable() && op2.get_variable())	  	  
	    m_bb.rem(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.rem(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::URem:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.urem(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.urem(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::And:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.bitwise_and(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.bitwise_and(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::Or:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.bitwise_or(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	    m_bb.bitwise_or(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::Xor:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.bitwise_xor(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.bitwise_xor(lhs, (*op1.get_variable()), op2.constant());	    
	return;
      case BinaryOperator::Shl:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.shl(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.shl(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::AShr:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.ashr(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.ashr(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      case BinaryOperator::LShr:
	if (op1.get_variable() && op2.get_variable())	  	  
	  m_bb.lshr(lhs, (*op1.get_variable()), (*op2.get_variable()));
	else if (op1.get_variable() && op2.is_constant())
	  m_bb.lshr(lhs, (*op1.get_variable()), op2.constant());	    
	return;	  
      default:;;
      }
      CLAM_ERROR("unexpected problem with binary operator");
    }

    
    void doArithmetic(crab_lit_ref_t ref, BinaryOperator &i) {
      if (!ref || !ref->isVar() || !(ref->isInt())) {
	CLAM_ERROR("lhs of arithmetic operation must be an integer");
      }
      var_t lhs = ref->getVar();

      const Value& v1 = *i.getOperand(0);
      const Value& v2 = *i.getOperand(1);
      
      crab_lit_ref_t ref1 = m_lfac.getLit(v1);
      if (!ref1 || !(ref1->isInt())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return;
      }

      crab_lit_ref_t ref2 = m_lfac.getLit(v2);
      if (!ref2 || !(ref2->isInt())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return;
      }
      
      lin_exp_t op1 = m_lfac.getExp(ref1);
      lin_exp_t op2 = m_lfac.getExp(ref2);

      if (op1.is_constant() && op2.is_constant()) {
	number_t n1 = op1.constant();
	number_t n2 = op2.constant();
	switch(i.getOpcode()) {
	case BinaryOperator::Add: //m_bb.assign(lhs, n1+n2); break;
	case BinaryOperator::Sub: //m_bb.assign(lhs, n1-n2); break;	
	case BinaryOperator::Mul: //m_bb.assign(lhs, n1*n2); break;
	case BinaryOperator::SDiv:
	case BinaryOperator::UDiv:
	case BinaryOperator::SRem:
	case BinaryOperator::URem:	  
	case BinaryOperator::Shl:
	case BinaryOperator::AShr:	  
	case BinaryOperator::LShr: {		 
	  var_t t1 = m_lfac.mkIntVar(i.getType()->getIntegerBitWidth());
	  var_t t2 = m_lfac.mkIntVar(i.getType()->getIntegerBitWidth());	  
	  m_bb.assign(t1, n1);
	  m_bb.assign(t2, n2);
	  doBinOp(i.getOpcode(), lhs, t1, t2);
	}
	  break;	  
	default:
	  // this should not happen
	  CLAM_ERROR("unexpected instruction");	  
	}
	return;
      }
	
      switch(i.getOpcode()) {
      case BinaryOperator::Add:
      case BinaryOperator::Sub:
      case BinaryOperator::Mul:
      case BinaryOperator::SDiv:
      case BinaryOperator::UDiv:
      case BinaryOperator::SRem:
      case BinaryOperator::URem:
      case BinaryOperator::Shl:
      case BinaryOperator::AShr:
      case BinaryOperator::LShr:		
	if (op1.is_constant()) { 
	  // Crab cfg does not support arithmetic operations between a
	  // constant and variable.
	  var_t t = m_lfac.mkIntVar(i.getType()->getIntegerBitWidth());	  
	  m_bb.assign(t, op1.constant());
	  doBinOp(i.getOpcode(), lhs, t, op2);
	} else {
	  doBinOp(i.getOpcode(), lhs, op1, op2);	  
	}
	break;
      default:
	// this should not happen
	CLAM_ERROR("unexpected instruction");
      }
    }
    
    var_t doBoolLogicOp(Instruction::BinaryOps op,
			/* ref can be null */
			crab_lit_ref_t ref, const Value& v1, const Value& v2) {
      
      if (ref && !(ref->isBool())) {
	CLAM_ERROR("lhs of arithmetic operation must be an Boolean");
      }
      
      var_t lhs = (ref ? ref->getVar() : m_lfac.mkBoolVar());
      
      crab_lit_ref_t b1 = m_lfac.getLit(v1);
      if (!b1 || !(b1->isBool())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return lhs;
      }

      crab_lit_ref_t b2 = m_lfac.getLit(v2);
      if (!b2 || !(b2->isBool())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return lhs;
      }
           
      switch(op) {
      case BinaryOperator::And:
	if (b1->isVar() && b2->isVar()) {
	  m_bb.bool_and(lhs, b1->getVar(), b2->getVar());
	} else if (!b1->isVar() && !b2->isVar()) {	
	  m_bb.bool_assign(lhs, m_lfac.isBoolTrue(b1) && m_lfac.isBoolTrue(b2)?
			    lin_cst_t::get_true() : lin_cst_t::get_false());
	} else if (m_lfac.isBoolFalse(b1) || m_lfac.isBoolFalse(b2)) {
	  m_bb.bool_assign(lhs, lin_cst_t::get_false());
	} else if (m_lfac.isBoolTrue(b1)) {
	  m_bb.bool_assign(lhs, b2->getVar());
	} else if (m_lfac.isBoolTrue(b2)) {
	  m_bb.bool_assign(lhs, b1->getVar());
	} else {
	  CLAM_ERROR("unexpected uncovered case in doBoolLogicOp And");
	}
	break;
      case BinaryOperator::Or:
	if (b1->isVar() && b2->isVar()) {
	  m_bb.bool_or(lhs, b1->getVar(), b2->getVar());
	} else if (!b1->isVar() && !b2->isVar()) {	 
	  m_bb.bool_assign(lhs,  m_lfac.isBoolTrue(b1) || m_lfac.isBoolTrue(b2)?
			    lin_cst_t::get_true(): lin_cst_t::get_false());
	} else if (m_lfac.isBoolTrue(b1) || m_lfac.isBoolTrue(b2)){
	  m_bb.bool_assign(lhs, lin_cst_t::get_true());
	} else if (m_lfac.isBoolFalse(b1)) {
	  m_bb.bool_assign(lhs, b2->getVar());
	} else if (m_lfac.isBoolFalse(b2)) {
	  m_bb.bool_assign(lhs, b1->getVar());
	} else {
	  CLAM_ERROR("unexpected uncovered case in doBoolLogicOp Or");
	}
	break;
      case BinaryOperator::Xor:
	if (b1->isVar() && b2->isVar()){
	  m_bb.bool_xor(lhs, b1->getVar(), b2->getVar());
	} else if (!b1->isVar() && !b2->isVar()) {	 
	  m_bb.bool_assign(lhs,(((m_lfac.isBoolTrue(b1)  && m_lfac.isBoolFalse(b2)) ||
				  (m_lfac.isBoolFalse(b1) && m_lfac.isBoolTrue(b2))) ?
				  lin_cst_t::get_true(): lin_cst_t::get_false()));
	} else if (m_lfac.isBoolTrue(b1)){
	  m_bb.bool_assign(lhs, b2->getVar(), true /*negate rhs*/);
	} else if (m_lfac.isBoolFalse(b1)){
	  m_bb.bool_assign(lhs, b2->getVar());
	} else if (m_lfac.isBoolTrue(b2)) {
	  m_bb.bool_assign(lhs, b1->getVar(), true /*negate rhs*/);
	} else if (m_lfac.isBoolFalse(b2)) {
	  m_bb.bool_assign(lhs, b1->getVar());	    
	} else {
	  CLAM_ERROR("unexpected uncovered case in doBoolLogicOp Xor");
	}
	break;
      default:
	CLAM_WARNING("translation skipped bool logic operation at line " << __LINE__);
	havoc(lhs, m_bb, m_params.include_useless_havoc);
      }
      return lhs;
    }

    void doIntLogicOp(crab_lit_ref_t ref, BinaryOperator &i) {
      assert(ref && ref->isVar());

      if (!(ref->isInt())) {
	CLAM_ERROR("lhs of bitwise operation must be an integer");
      }
      var_t lhs = ref->getVar();

      const Value& v1 = *i.getOperand(0);
      const Value& v2 = *i.getOperand(1);

      crab_lit_ref_t ref1 = m_lfac.getLit(v1);
      if (!ref1 || !(ref1->isInt())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return;
      }

      crab_lit_ref_t ref2 = m_lfac.getLit(v2);
      if (!ref2 || !(ref2->isInt())) {
	havoc(lhs, m_bb, m_params.include_useless_havoc);
	return;
      }
      
      lin_exp_t op1 = m_lfac.getExp(ref1);
      lin_exp_t op2 = m_lfac.getExp(ref2);

      switch(i.getOpcode()) {
        case BinaryOperator::And:
        case BinaryOperator::Or:
        case BinaryOperator::Xor:
	  doBinOp(BinaryOperator::And, lhs, op1, op2);
	  break;
        default:
	  CLAM_WARNING("translation skipped " << i << " at line " << __LINE__);
          havoc(lhs, m_bb, m_params.include_useless_havoc);
      }
    }

    void doAllocFn(Instruction &I) {

      if (!I.getType()->isVoidTy()) {
	crab_lit_ref_t ref = m_lfac.getLit(I);
	assert(ref->isVar());
	if (isPointer(I, m_params)) {
	  m_bb.ptr_new_object(ref->getVar(), m_object_id++);
	} else if (isTracked(I, m_params)) {
	  // -- havoc return value	  
	  havoc(ref->getVar(), m_bb, m_params.include_useless_havoc);
	}
      }

      /**
       * TODO: add an array_init statement for the allocation function.
       * This would be unsound in general so we need to be careful.
       **/
      
      // if (m_params.enabled_aggressive_array_initialization()) {
      //   mem_region_t r = GET_REGION(I,&I);
      //   bool isMainCaller = I.getParent()->getParent()->getName().equals("main");
      //   if (isMainCaller && !r.isUnknown()) {
      //      
      //      // We need to figure out:
      //      // - the number of elements and the size of each element
      //      // - Otherwise, we create a fresh (unbounded) variable and use it
      //      //   for the upper bound.
      //   }
      // }
    }

    void doMemIntrinsic(MemIntrinsic& I) {

      if (m_lfac.get_track() == NUM) {
	return;
      } else if (m_lfac.get_track() == PTR) {
	// XXX: memory intrinsics are currently only translated for ARR
	CLAM_WARNING("Skipped memory intrinsics " << I);
	return;
      }

      Function& parent = *(I.getParent()->getParent());      
      if (MemCpyInst *MCI = dyn_cast<MemCpyInst>(&I)) {
	Value* src = MCI->getSource();
	Value* dst = MCI->getDest();
	mem_region_t src_reg = get_region(m_mem, parent, src);
	mem_region_t dst_reg = get_region(m_mem, parent, dst); 
	if (dst_reg.isUnknown() || src_reg.isUnknown()) return;
	m_bb.havoc(m_lfac.mkArrayVar(dst_reg));
	if (dst_reg.get_type() == src_reg.get_type()) {
	  m_bb.array_assign(m_lfac.mkArrayVar(dst_reg), m_lfac.mkArrayVar(src_reg));
	}
      } else if (MemSetInst *MSI = dyn_cast<MemSetInst>(&I)) {
	if (m_params.enabled_aggressive_array_initialization() && isInteger(*(MSI->getValue()))) {
	  Value* dst = MSI->getDest();
	  mem_region_t r = get_region(m_mem, parent, dst);
	  if (r.isUnknown()) return;

	  crab_lit_ref_t len_ref = m_lfac.getLit(*(MSI->getLength()));
	  crab_lit_ref_t val_ref = m_lfac.getLit(*(MSI->getValue()));
	  if (!len_ref || !val_ref) return;
	  
	  if (len_ref->isInt()) {
	    lin_exp_t lb_idx(number_t(0));
	    lin_exp_t ub_idx(m_lfac.getExp(len_ref) -1);
	    var_t arr_var = m_lfac.mkArrayVar(r);
	    uint64_t elem_size = MSI->getAlignment(); /*double check this*/
	    if (val_ref->isInt()) {
	      if (m_init_regions.insert(r).second) {
		if (val_ref->isVar()) {
		  m_bb.array_init(arr_var, lb_idx, ub_idx, val_ref->getVar(), elem_size);
		} else {
		  m_bb.array_init(arr_var, lb_idx, ub_idx,
				  m_lfac.getIntCst(val_ref), elem_size);
		}
	      }
	    } else if (val_ref->isBool()) {
	      if (m_init_regions.insert(r).second) {	      
		if (val_ref->isVar()) {
		  m_bb.array_init(arr_var, lb_idx, ub_idx, val_ref->getVar(), elem_size);
		} else {
		  m_bb.array_init(arr_var, lb_idx, ub_idx,
				  m_lfac.isBoolTrue(val_ref) ? number_t(1): number_t(0),
				  elem_size);
		}
	      }
	    } else if (val_ref->isPtr()) {
	      /** This should not happen since we ignore array of pointers **/
	      m_bb.havoc(arr_var);	      
	    }
	  }
	} else {
	  if (!isInteger(*(MSI->getValue()))) {
	    CLAM_WARNING("Skipped memset instruction of non-integer type.");
	  } else {
	    CLAM_WARNING("Skipped memset instruction of integer type. " <<
			     "You can enable --crab-unsound-array-init on your own risk.");
	  }
	}
      } else if (isa<MemMoveInst>(&I)) {
	CLAM_WARNING("Skipped memmove instruction");
      }
    }

    /* verifier.zero_initializer(v) or verifier.int_initializer(v,k) */    
    void doInitializer(CallInst &I) {
      CallSite CS(&I);
      // v is either a global variable or a gep instruction that
      // indexes an address inside the global variable.
      Value *v = CS.getArgument(0);
      Type* ty = cast<PointerType>(v->getType())->getElementType();
      Function& parent = *(I.getParent()->getParent());
      
      auto r = get_region(m_mem, parent, v);
      if (!r.isUnknown()) {
	crab_lit_ref_t ref = nullptr;
	if (CS.arg_size() == 2) {
	  ref= m_lfac.getLit(*(CS.getArgument(1)));
	  if (!ref) { // this can happen if k is bignum and bignums are not allowed
	    return;
	  }
	}
	if (isGlobalSingleton(r, m_params.lower_singleton_aliases)) {
	  // Promote the global to an integer/boolean scalar
	  var_t s = m_lfac.mkArraySingletonVar(r);
	  if (isInteger(ty)) {
	    number_t init_val = ((CS.arg_size() == 2) && !ref->isVar() ?
				 m_lfac.getIntCst(ref) : number_t(0));
	    m_bb.assign(s, init_val);
	  } else if (isBool(ty)) {
	    lin_cst_t init_val = ((CS.arg_size() == 2) &&
				  !ref->isVar() && m_lfac.isBoolTrue(ref) ?
				  lin_cst_t::get_true() : lin_cst_t::get_false());
	    m_bb.bool_assign(s, init_val);
	  } else { /* unreachable*/ }
	} else {
	  number_t init_val(0);
	  lin_exp_t lb_idx(number_t(0));
	  lin_exp_t ub_idx(number_t(0));
	  uint64_t elem_size = storageSize(ty);
	  var_t a = m_lfac.mkArrayVar(r);

	  /* verifier.int_initializer(v,k) */
	  if (CS.arg_size() == 2) { 
	    if (ref->isInt()) {
	      init_val = m_lfac.getIntCst(ref);
	    } else if (ref->isBool()) {
	      if (m_lfac.isBoolTrue(ref)) init_val = number_t(1);
	    } else {
	      // unreachable
	      CLAM_ERROR("second argument of verifier.int_initializer must be int or bool");
	    }
	  }

	  /* verifier.zero_initializer(v) */
	  if (isInteger(ty) || isBool(ty)) {
	    if (m_init_regions.insert(r).second) {	      	    
	      IntegerType* int_ty = cast<IntegerType>(ty);
	      ub_idx = (isBool(ty) ? 0 : z_number((int_ty->getBitWidth() / 8) -1));
	      m_bb.array_init(a, lb_idx, ub_idx, init_val, elem_size);
	    }
	  } else if (isIntArray(*ty) || isBoolArray(*ty)) {
	    if (cast<ArrayType>(ty)->getNumElements() == 0) {
	      // TODO: zero-length array are possible inside structs We
	      // can simply make ub_idx > 0.  However, DSA is very
	      // likely that it will collapse anyway so the fact we skip
	      // the translation won't make any difference.
	      CLAM_WARNING("translation skipped a zero-length array");
	    } else {
	      if (m_init_regions.insert(r).second) {	      	    	      
		elem_size = storageSize(cast<ArrayType>(ty)->getElementType());
		ub_idx = lin_exp_t(cast<ArrayType>(ty)->getNumElements()* elem_size - 1);
		m_bb.array_init(a, lb_idx, ub_idx, init_val, elem_size);
	      }
	    }
	  } else { /** unreachable **/ }
	}
      }
    }
    
    void doVerifierCall(CallInst &I) {
      CallSite CS(&I);

      const Value *calleeV = CS.getCalledValue();
      const Function *callee = dyn_cast<Function>(calleeV->stripPointerCasts());
      if (!callee) return;
            
      if (isErrorFn(callee)) {
        m_bb.assertion(lin_cst_t::get_false(), getDebugLoc(&I));
        return;
      }

      if (isSeaHornFail(callee)) {
	// when seahorn inserts a call to "seahorn.fail" means that
	// the program is safe iff the function cannot return.  Note
	// that we cannot add "assert(false)" in the current
	// block. Instead, we need to check whether the exit block of
	// the function is reachable or not.
	m_has_seahorn_fail = true;
	return;
      }

      if (!isAssertFn(callee) && !isAssumeFn(callee) && !isNotAssumeFn(callee))
	return; 

      Value *cond = CS.getArgument(0);
      
      if (!isTracked(*cond, m_params)) return;
      
      if (ConstantInt *CI = dyn_cast<ConstantInt>(cond)) {
        // -- cond is a constant
	bool is_bignum;
	z_number cond_val = getIntConstant(CI, m_params, is_bignum);
	if (!is_bignum) {
	  if (cond_val > 0) {
	    if (isAssertFn(callee) || isAssumeFn(callee)) {
	      // do nothing
	    } else {
	      assert(isNotAssumeFn(callee));
	      m_bb.assume(lin_cst_t::get_false()); 	      
	    }
	  } else {
	    if (isNotAssumeFn(callee)) {
	      // do nothing
	    } else if (isAssumeFn(callee)) {
	      m_bb.assume(lin_cst_t::get_false());
	    } else {
	      assert(isAssertFn(callee));
	      m_bb.assertion(lin_cst_t::get_false(), getDebugLoc(&I));
	    }
	  }
	}
      } else {
	crab_lit_ref_t cond_ref = m_lfac.getLit(*cond);
	assert(cond_ref->isVar());
	var_t v = cond_ref->getVar();
	// -- cond is variable
	if (cond_ref->isBool()) {
	  if (isNotAssumeFn(callee))
	    m_bb.bool_not_assume(v);
	  else if (isAssumeFn(callee))
	    m_bb.bool_assume(v);
	  else  {
	    assert(isAssertFn(callee));
	    m_bb.bool_assert(v, getDebugLoc(&I));
	  }
	} else if (cond_ref->isInt()){

	  ZExtInst *ZEI = dyn_cast<ZExtInst>(cond);
	  if (ZEI && ZEI->getSrcTy()->isIntegerTy(1)) {
	    /* Special case to replace this pattern:
	         y:i32 = zext x:i1 to i32 
	         assume (y>=1);
               with 
	         bool_assume(x);
                 This can help boolean/numerical propagation in the crab domains.
	    */
	    cond_ref = m_lfac.getLit(*(ZEI->getOperand(0)));
	    assert(cond_ref->isVar()); // boolean variable
	    v = cond_ref->getVar();
	    if (isNotAssumeFn(callee)) {	   
	      m_bb.bool_not_assume(v);	   
	    } else if (isAssumeFn(callee)) {
	      m_bb.bool_assume(v);
	    } else  {
	      assert(isAssertFn(callee));
	      m_bb.bool_assert(v, getDebugLoc(&I));
	    }
	  } else {
	    if (isNotAssumeFn(callee)) {	   
	      m_bb.assume(v <= number_t(0));	   
	    } else if (isAssumeFn(callee)) {
	      m_bb.assume(v >= number_t(1));
	    } else  {
	      assert(isAssertFn(callee));
	      m_bb.assertion(v >= number_t(1), getDebugLoc(&I));
	    }
	  }
	}
      }
    }

   public:

    CrabInstVisitor(crabLitFactory &lfac, HeapAbstraction &mem,
      const DataLayout* dl, const TargetLibraryInfo* tli,
      basic_block_t &bb, 
      llvm::DenseMap<const statement_t*, const llvm::Instruction*> &rev_map,
      std::set<mem_region_t>& init_regions,		    
      const CrabBuilderParams &params)
      : m_lfac(lfac)
      , m_mem(mem)
      , m_dl(dl)
      , m_tli(tli)
      , m_bb(bb)
      , m_object_id(0)
      , m_has_seahorn_fail(false)
      , m_rev_map(rev_map)
      , m_init_regions(init_regions)
      , m_params(params) {}

    bool has_seahorn_fail() const { return m_has_seahorn_fail;}

    /// skip PHI nodes (processed elsewhere)
    void visitPHINode(PHINode &I) {}

    /// skip BranchInst (processed elsewhere)
    void visitBranchInst(BranchInst &I) {}

    /// skip SwitchInst (processed elsewhere)
    void visitSwitchInst(SwitchInst& I) {}
    
    /// I is already translated if it is the condition of a branch or
    /// a select's condition.  Here we cover cases where I is an
    /// operand of other instructions.
    void visitCmpInst(CmpInst &I) {
      
      if (!isTracked(I, m_params)) return;

      crab_lit_ref_t ref = m_lfac.getLit(I);
      assert(ref->isVar());
      
      if (isPointer(*I.getOperand(0),m_params) &&
	  isPointer(*I.getOperand(1),m_params)) {
	
	if (!AllUsesAreBrInst(&I)) {
	  CLAM_WARNING("translation skipped comparison between pointers");
	  havoc(ref->getVar(), m_bb, m_params.include_useless_havoc);		  
	}
	return;
      }
      
      // make sure we only translate if both operands are integers or booleans
      if (!I.getOperand(0)->getType()->isIntegerTy() ||
	  !I.getOperand(1)->getType()->isIntegerTy()) {
	havoc(ref->getVar(), m_bb, m_params.include_useless_havoc);	
	return;
      }

      const Value& v0 = *(I.getOperand(0));
      const Value& v1 = *(I.getOperand(1));
      
      if (isBool(v0) && isBool(v1)) {
	// we lower it here
	if (I.getPredicate() == CmpInst::ICMP_EQ) { // eq <-> not xor
	  var_t tmp = doBoolLogicOp(BinaryOperator::Xor, nullptr, v0, v1);
	  m_bb.bool_assign(ref->getVar(), tmp, true); // not(tmp)
	} else if (I.getPredicate() == CmpInst::ICMP_NE) { // ne <-> xor
	  doBoolLogicOp(BinaryOperator::Xor, ref, v0, v1);
	} else {	    
	  CLAM_WARNING("translation skipped " << I << " at line " << __LINE__ );
	}
      } else {
	assert(isInteger(v0) && isInteger(v1));
	if (AllUsesAreBrOrIntSelectCondInst(&I)) {
	  // do nothing: already lowered elsewhere
	} else {
	  cmpInstToCrabBool(I, m_lfac, m_bb);	  
	}
      }
    }
    
    void visitBinaryOperator(BinaryOperator &I) {
      if (!isTracked(I, m_params)) return;
      
      crab_lit_ref_t ref = m_lfac.getLit(I);
      if (!ref || !(ref->isVar())) {
	CLAM_ERROR("unexpected lhs of binary operator");
      }
      
      switch (I.getOpcode()) {
      case BinaryOperator::Add:
      case BinaryOperator::Sub:
      case BinaryOperator::Mul:
      case BinaryOperator::SDiv:
      case BinaryOperator::UDiv:
      case BinaryOperator::SRem:
      case BinaryOperator::URem:
      case BinaryOperator::Shl:
      case BinaryOperator::AShr:
      case BinaryOperator::LShr:	
	doArithmetic(ref, I);
	break;
      case BinaryOperator::And:
      case BinaryOperator::Or:
      case BinaryOperator::Xor:
	if (isBool(I))
	  doBoolLogicOp(I.getOpcode(), ref, *I.getOperand(0), *I.getOperand(1));
	else
	  doIntLogicOp(ref, I);
	break;
      default:
	havoc(ref->getVar(), m_bb, m_params.include_useless_havoc);
      }
    }
        
    void visitCastInst(CastInst &I) {
      if (!isTracked(I, m_params)) return;

      // XXX: Once we respect integer bitwidths, this optimization
      // cannot be applied.
      // if ((isa<SExtInst>(I) || isa<ZExtInst> (I)) && AllUsesAreGEP(&I)) {
      // 	/* 
      // 	 *  This optimization tries to reduce the number variables within
      // 	 *  a basic block. This will put less pressure on the numerical
      // 	 *  abstract domain later on. Search for this idiom: 
      // 	 *  %_14 = zext i8 %_13 to i32
      // 	 *  %_15 = getelementptr inbounds [10 x i8]* @_ptr, i32 0, i32 %_14
      // 	 */
      // 	return;
      // }
      
      if (AllUsesAreNonTrackMem(&I) || AllUsesAreIndirectCalls(&I)) {
	return;
      }

      if (isa<ZExtInst>(I) && I.getSrcTy()->isIntegerTy(1) && AllUsesAreVerifierCalls(&I)) {
	/* 
	   y:i32 = zext x:i1 to i32 
	   assume (y>=1);
	*/
	return;
      }

      crab_lit_ref_t dst = m_lfac.getLit(I);
      assert(dst && dst->isVar());
      crab_lit_ref_t src = m_lfac.getLit(*(I.getOperand(0)));
      if (!src) {
	havoc(dst->getVar(), m_bb, m_params.include_useless_havoc);
	return;
      }
      
      // -- INTEGER OR BOOLEAN CAST
      if (isIntCast(I)) {
	if (I.getSrcTy() == I.getDestTy()) {
	  // assume the frontend removes useless casts.
	  CLAM_WARNING("translation does not support non-op integer casts");
	  havoc(dst->getVar(), m_bb, m_params.include_useless_havoc);
	} else {
	  if (!src->isVar()) {
	    // We store the constant into a variable
	    if (src->isBool()) {
	      var_t tmp = m_lfac.mkBoolVar();
	      m_bb.bool_assign(tmp, m_lfac.isBoolTrue(src) ?
			       lin_cst_t::get_true() : lin_cst_t::get_false());
	      if (isa<SExtInst>(I)) {		    
		m_bb.sext(tmp, dst->getVar());
	      } else if (isa<ZExtInst>(I)) {
		m_bb.zext(tmp, dst->getVar());
	      } else {
		CLAM_ERROR("unexpected cast operation on Booleans"); 
	      }
	    } else if (src->isInt()) {
	      var_t tmp = m_lfac.mkIntVar(I.getOperand(0)->getType()->getIntegerBitWidth());
	      m_bb.assign(tmp, m_lfac.getIntCst(src));
	      if (isa<SExtInst>(I)) {
		m_bb.sext(tmp, dst->getVar());
	      } else if (isa<ZExtInst>(I)) {
		m_bb.zext(tmp, dst->getVar());
	      } else if (isa<TruncInst>(I)) {
		m_bb.truncate(tmp, dst->getVar());
	      } else {
		CLAM_ERROR("unexpected cast operation");
	      }
	    } else {
	      CLAM_ERROR("unexpected cast operand type"); 
	    }
	  } else {
	    if (isa<SExtInst>(I)) {
	      m_bb.sext(src->getVar(), dst->getVar());
	    } else if (isa<ZExtInst>(I)) {
	      m_bb.zext(src->getVar(), dst->getVar());
	    } else if (isa<TruncInst>(I)) {
	      m_bb.truncate(src->getVar(), dst->getVar());
	    } else {
	      CLAM_ERROR("unexpected cast operation");
	    }
	  }
	}
	return;
      }

      // -- POINTER CAST      
      if (isPointerCast(I)) {
	if (isa<PtrToIntInst>(I)) {
	  //CLAM_WARNING("translation skipped pointer to integer cast");
	} else if (isa<IntToPtrInst>(I)) {
	  //CLAM_WARNING("translation skipped integer to pointer cast");
	} else if (isa<BitCastInst>(I) && isPointer(*I.getOperand(0), m_params)) {

	  if (src->isPtr()) {
	    if (m_lfac.isPtrNull(src)) {
	      m_bb.ptr_null(dst->getVar());
	    } else {
	      assert (src->isVar());
	      m_bb.ptr_assign(dst->getVar(), src->getVar(), number_t(0));
	    }
	    return;
	  }
	  CLAM_WARNING("translation skipped " << I << " at line " << __LINE__);	  
	}
      }
      havoc(dst->getVar(), m_bb, m_params.include_useless_havoc);
    }

    // Analysis of select instructions is cumbersome since it requires
    // a sequence of assume and join operations. Moreover, if many
    // select instructions appear in the same block its analysis can
    // be very inefficient due to the high number of joins.
    // 
    // If possible the simplest solution is to get rid of select
    // instructions. This can be done by adding option
    // --lower-select. This option will remove select instructions at
    // the expense of adding new basic blocks although hopefully the
    // llvm frontend will simplify them. If this is not possible or
    // undesirable then we try to deal with the select instruction
    // here.
    void visitSelectInst(SelectInst &I) {
      if (!isTracked(I, m_params)) return;

      crab_lit_ref_t lhs = m_lfac.getLit(I);
      assert(lhs && lhs->isVar());
      
      if (isPointer(I, m_params)) {
	// We don't even bother with pointers
	CLAM_WARNING("skipped " << I << "\n" << "Enable --lower-select.");
	havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	return;
      }
      
      Value& cond = *I.getCondition();
      crab_lit_ref_t c = m_lfac.getLit(cond);
      assert(c);
      crab_lit_ref_t op1 = m_lfac.getLit(*I.getTrueValue());
      assert(op1);
      crab_lit_ref_t op2 = m_lfac.getLit(*I.getFalseValue());
      assert(op2);
      
      if (isBool(I)) {
	// --- All operands are BOOL
	if (!op1->isBool()) {
	  havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  return;
	}
	if (!op2->isBool()) {
	  havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  return;
	}	
	
	// -- simple cases first: we know the condition is either true or false
	if (ConstantInt *ci = dyn_cast<ConstantInt>(&cond)) {
	  if (ci->isOne()) {
	    if (!op1->isVar()) {
	      m_bb.bool_assign(lhs->getVar(),(m_lfac.isBoolTrue(op1) ?
					       lin_cst_t::get_true():lin_cst_t::get_false()));
	    } else { 
	      m_bb.bool_assign(lhs->getVar(), op1->getVar());
	    }
	  } else {
	    if (!ci->isZero())
	      CLAM_ERROR("unexpected select condition");
	    if (!op2->isVar()) {
	      m_bb.bool_assign(lhs->getVar(),(m_lfac.isBoolTrue(op2) ?
					       lin_cst_t::get_true(): lin_cst_t::get_false()));
	    } else {
	      m_bb.bool_assign(lhs->getVar(), op2->getVar());
	    }
	  }
	  return;
	}

	assert(c->isVar());
	
	// -- general case: we don't know whether condition is true or not.
	if (!op1->isVar() && !op2->isVar()) {
	  var_t tt_v = m_lfac.mkBoolVar();
	  var_t ff_v = m_lfac.mkBoolVar();
	  m_bb.bool_assign(tt_v, (m_lfac.isBoolTrue(op1)?
				  lin_cst_t::get_true(): lin_cst_t::get_false()));
	  m_bb.bool_assign(ff_v, (m_lfac.isBoolTrue(op2)?
				  lin_cst_t::get_true(): lin_cst_t::get_false()));
	  m_bb.bool_select(lhs->getVar(), c->getVar(), tt_v, ff_v);	
	} else if (!op1->isVar()) {
	  var_t tt_v = m_lfac.mkBoolVar();
	  m_bb.bool_assign(tt_v, (m_lfac.isBoolTrue(op1)?
				  lin_cst_t::get_true(): lin_cst_t::get_false()));
	  m_bb.bool_select(lhs->getVar(), c->getVar(), tt_v, op2->getVar());	
	} else if(!op2->isVar()) {
	  var_t ff_v = m_lfac.mkBoolVar();
	  m_bb.bool_assign(ff_v, (m_lfac.isBoolTrue(op2)?
				  lin_cst_t::get_true(): lin_cst_t::get_false()));
	  m_bb.bool_select(lhs->getVar(), c->getVar(), op1->getVar(), ff_v);	
	} else {
	  m_bb.bool_select(lhs->getVar(), c->getVar(), op1->getVar(), op1->getVar());
	}
      } else if (isInteger(I)) {
	
	// --- All operands except the condition are INTEGERS
	if (!op1->isInt()) {
	  havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  return;
	}
	if (!op2->isInt()) {
	  havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  return;
	}	
	
	lin_exp_t e1 = m_lfac.getExp(op1);
	lin_exp_t e2 = m_lfac.getExp(op2);
	
	// -- simple cases first: we know the condition is either true or false
	if (ConstantInt *ci = dyn_cast<ConstantInt>(&cond)) {
	  if (ci->isOne()) {
	    m_bb.assign(lhs->getVar(), e1);
	  } else { 
	    if (!ci->isZero())
	      CLAM_ERROR("Unexpected select condition");
	    m_bb.assign(lhs->getVar(), e2);
	  }
	  return;
	}

	assert(c->isVar());
	
	// -- general case: we don't know whether the condition is true or not
	if (CmpInst* CI = dyn_cast<CmpInst>(&cond)) {
	  if (auto cst_opt = cmpInstToCrabInt(*CI, m_lfac)) {
	    m_bb.select(lhs->getVar(), *cst_opt, e1, e2);
	    return;
	  }
	}

	// The condition is a boolean but neither select or
	// bool_select are the right choice. The latter is only when
	// all operands are booleans. The former will have this form
	// (select (x:= cond >=1 ? e1: e2). This will be propagated
	// only to numerical domain which doesn't know anything about
	// cond. One solution is to zext cond to an integer. But maybe
	// another solution is to allow select to be a variable rather
	// than constraint.
        #if 1
	var_t icond = m_lfac.mkIntVar(8 /*any bitwdith >1*/);
	m_bb.zext(c->getVar(), icond);
	m_bb.select(lhs->getVar(), icond, e1, e2);
        #else
	CLAM_WARNING("skipped " << I << "\n" <<
			 "Crab select does not support natively boolean conditions.\n" << 
			 "Meanwhile, enable --lower-select or --crab-bool-as-int");
	havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
        #endif 
      } 
    }
    
    void visitGetElementPtrInst(GetElementPtrInst &I) {
      if (!isTracked(I, m_params)) return;
      
      CRAB_LOG("cfg-gep", llvm::errs() << "Translating " << I << "\n");

      Function& parent = *(I.getParent()->getParent());
      mem_region_t r = get_region(m_mem, parent, &I); 
      if (isGlobalSingleton(r, m_params.lower_singleton_aliases)) {
	CRAB_LOG("cfg-gep", llvm::errs() << "Skipped singleton region\n");
	return;
      }

      crab_lit_ref_t lhs = m_lfac.getLit(I);
      assert(lhs && lhs->isVar());
      crab_lit_ref_t ptr = m_lfac.getLit(*I.getPointerOperand());
      
      if (!ptr) {
        havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
        return;
      }
      
      if (m_lfac.isPtrNull(ptr)) {
	CLAM_WARNING(I << " doing pointer arithmetic with null pointer.");
        havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
        return;
      }
      assert(ptr->isVar());
      
      // -- translation if the GEP offset is constant
      unsigned bitwidth = m_dl->getPointerTypeSizeInBits(I.getType());
      APInt offset(bitwidth, 0);
      if (I.accumulateConstantOffset(*m_dl, offset)) {
	bool is_bignum = false;
	z_number o(toZNumber(offset, m_params, is_bignum));
	if (is_bignum) {
	  m_bb.havoc(lhs->getVar());
	} else {
	  m_bb.ptr_assign(lhs->getVar(), ptr->getVar(), lin_exp_t(o));
	  CRAB_LOG("cfg-gep",
	   	   crab::outs() << "-- " << *lhs << ":=" << *ptr  << "+"
		                << o << "\n");
	}
        return;
      }

      // -- translation if symbolic GEP offset
      // If here, we know that there is at least one non-zero, symbolic index.
      bool already_assigned = false;
      for(auto GTI = gep_type_begin(&I), GTE = gep_type_end(&I); GTI != GTE; ++GTI) {
	if (const StructType *st = GTI.getStructTypeOrNull()) {
	  if (const ConstantInt *ci = dyn_cast<const ConstantInt>(GTI.getOperand())) {
            number_t offset (fieldOffset (st, ci->getZExtValue ()));
	    m_bb.ptr_assign (lhs->getVar(), (!already_assigned) ?
			     ptr->getVar() : lhs->getVar(), offset);
	    CRAB_LOG("cfg-gep",
		     if (!already_assigned) {
		       crab::outs() << *lhs << ":=" << *ptr << "+" << offset << "\n";
		     } else {
		       crab::outs() << *lhs << ":=" << *lhs << "+" << offset << "\n";
		     }); 
	    already_assigned = true;
	  } else {
            CLAM_ERROR("GEP index expected only to be an integer"); 
	  }
	} else {
	  // otherwise we have a sequential type like an array or vector.
	  // Multiply the index by the size of the indexed type.
          if (const ConstantInt *ci = dyn_cast<const ConstantInt> (GTI.getOperand())) {
            if (ci->isZero ())
              continue;
          }
	  crab_lit_ref_t idx = m_lfac.getLit(*GTI.getOperand()); 
	  if (!idx || !idx->isInt()){
	    CLAM_ERROR("unexpected GEP index");
	  }
	  lin_exp_t offset(m_lfac.getExp(idx) *
			   number_t(storageSize(GTI.getIndexedType())));
	  m_bb.ptr_assign(lhs->getVar(), (!already_assigned) ?
			  ptr->getVar() : lhs->getVar(), offset);
	  CRAB_LOG("cfg-gep",
		   if (!already_assigned) {
		     crab::outs() << *lhs << ":=" << *ptr << "+" << offset << "\n";
		   } else {
		     crab::outs() << *lhs << ":=" << *lhs << "+" << offset << "\n";
		   }); 
	  already_assigned = true;
	}
      }
    }

    void visitStoreInst(StoreInst &I) {
      /* The LLVM store instruction will be translated to EITHER:

	 a) crab array store, or
	 b) crab pointer store

	 If the type of the stored value is integer or boolean then it
	 will be interpreted as an array store.

	 If the type of the stored value is a pointer then it will be
	 interpreted as a pointer store.

	 Otherwise, e.g., a store of a floating point or vector type, it will
	 ignored.
      */

      if (isa<ConstantExpr>(I.getPointerOperand()) ||
	  isa<ConstantExpr>(I.getValueOperand())) {
	// We don't handle constant expressions. 
	return;
      }

      crab_lit_ref_t ptr = m_lfac.getLit(*I.getPointerOperand());
      crab_lit_ref_t val = m_lfac.getLit(*I.getValueOperand());
      Function& func = *(I.getParent()->getParent());
      
      if (!ptr || !ptr->isPtr()) {
	CLAM_ERROR("unexpected pointer operand of store instruction");
      }
      
      if (m_lfac.isPtrNull(ptr)) {
	CLAM_WARNING(I << " is possibly dereferencing a null pointer");
	return;
      }
      
      if (m_lfac.get_track() == ARR &&
	  (isInteger(*I.getValueOperand()) || isBool(*I.getValueOperand()))){
	// -- value is an integer/bool -> add array statement
	if (!val) {
	  // XXX: this can happen if we store a ptrtoint instruction
	  // For simplicity, we don't deal with this case here and we
	  // assume that the client must make sure that all constant
	  // expressions are lowered. 
	  CLAM_ERROR("unexpected value operand of store instruction");
	}
	mem_region_t r = get_region(m_mem, func, I.getPointerOperand()); 
	if (!r.isUnknown()) {
	  if (isGlobalSingleton(r, m_params.lower_singleton_aliases)) {
	    // Promote the global to an integer/boolean scalar
	    var_t s = m_lfac.mkArraySingletonVar(r);
	    if (isInteger(*I.getValueOperand())) {
	      assert(val->isInt());
	      m_bb.assign(s, m_lfac.getExp(val));
	    } else if (isBool(*I.getValueOperand())) {
	      assert(val->isBool());
	      if (!val->isVar()) {
		m_bb.bool_assign(s, (m_lfac.isBoolTrue(val) ?
				     lin_cst_t::get_true() : lin_cst_t::get_false()));
	      } else { 
		m_bb.bool_assign(s, val->getVar(), false);
	      }
	    } else { /* unreachable */ }
	  } else {
	    Type* ty = I.getOperand(0)->getType();
	    /** 
	     * TODO: We completely forget the array index. This is ok
	     * for array smashing but it will be too imprecise for
	     * other array domains. We need to perform static analysis
	     * to identify for a given pointer its offset wrt to its
	     * allocation site.
	     **/
	    var_t idx = get_unconstrained_array_index_variable(m_lfac.get_vfac());

	    /** 
	     * We can help the array domain if we know already that
	     * the array store is a strong update. 
	     **/
	    bool is_uninit_region = m_init_regions.insert(r).second;	    
	    bool is_strong_update = r.getSingleton() || 
	      (func.getName() == "main" && 
	       (&(func.getEntryBlock()) == I.getParent()) &&
	       is_uninit_region);

	    if (val->isVar()) {
	      var_t t = val->getVar();
	      // Due to heap abstraction imprecisions, it can happen
	      // that the region's bitwidth is smaller than value's
	      // bitwidth.
	      if (r.get_bitwidth() < val->getVar().get_bitwidth()) {
		t = m_lfac.mkIntVar(r.get_bitwidth());
		// XXX: this truncate operation can overflow but the
		// store instruction does not overflow
		m_bb.truncate(val->getVar(), t);
	      }
	      auto const* crab_stmt =
		m_bb.array_store(m_lfac.mkArrayVar(r), idx, t, 
				 m_dl->getTypeAllocSize(ty), is_strong_update);
	      insert_rev_map(crab_stmt, I);
	    } else {
	      if (val->isInt()) {
		auto const* crab_stmt =
		  m_bb.array_store(m_lfac.mkArrayVar(r), idx, m_lfac.getIntCst(val),
				   m_dl->getTypeAllocSize(ty), is_strong_update);
		insert_rev_map(crab_stmt, I);
	      } else if (val->isBool()){
		auto const* crab_stmt =
		  m_bb.array_store(m_lfac.mkArrayVar(r), idx, 
				   m_lfac.isBoolTrue(val) ? number_t(1): number_t(0),
				   m_dl->getTypeAllocSize(ty), is_strong_update);
		insert_rev_map(crab_stmt, I);
	      } else { /* unreachable */}
	    }
	  }
	}
      } // else if (isPointer(*I.getPointerOperand(), m_params)) {
	// if (!val) {
	//   // this can happen e.g., with store double %_10, double* %_11
	//   // do nothing since we ignore floating point operations.
	// } else if (!val->isPtr()) {
	//   CLAM_ERROR("expecting a value operand of pointer type in store instruction");
	// } else {
	//   if (!m_lfac.isPtrNull(val)) {
	//     // XXX: we ignore the case if we store a null pointer. In
	//     // most cases, it will be fine since typical pointer
	//     // analyses ignore that case but it might be imprecise with
	//     // certain analyses.
	//     m_bb.ptr_store(ptr->getVar(), val->getVar());
	//   }
	// }
      else if (isPointer(*I.getValueOperand(), m_params)) {
	if (!val || !val->isPtr()) {
	  CLAM_ERROR("expecting a value operand of pointer type in store instruction");
	}
	
	if (!m_lfac.isPtrNull(val)) {
	  // XXX: we ignore the case if we store a null pointer. In
	  // most cases, it will be fine since typical pointer
	  // analyses ignore that case but it might be imprecise with
	  // certain analyses.
	  m_bb.ptr_store(ptr->getVar(), val->getVar());
	}      
      }
    }

    void visitLoadInst(LoadInst &I) {
      /*
	This case is symmetric to StoreInst.
       */

      if (!isTracked(I, m_params)) {
	return;
      }
      
      crab_lit_ref_t lhs = m_lfac.getLit(I);
      assert(lhs);
      
      if (isa<ConstantExpr>(I.getPointerOperand())) {
	// We don't handle constant expressions.
	havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	return;
      }
      
      crab_lit_ref_t ptr = m_lfac.getLit(*I.getPointerOperand());
      Function& func = *(I.getParent()->getParent());
      
      if (!ptr || !ptr->isPtr()) {
	CLAM_ERROR("unexpected pointer operand of load instruction");
      }
      
      if (m_lfac.isPtrNull(ptr)) {
	CLAM_WARNING(I << " is possibly dereferencing a null pointer");
	havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	return;
      }

      if (m_lfac.get_track() == ARR &&(isInteger(I) || isBool(I))) {
	// -- lhs is an integer/bool -> add array statement
	if (!lhs || !lhs->isVar()) {
	  CLAM_ERROR("unexpected lhs of load instruction");
	} 	
	mem_region_t r = get_region(m_mem, func, I.getPointerOperand()); 
	if (!(r.isUnknown())) {
	  if (isGlobalSingleton(r, m_params.lower_singleton_aliases)) {
	    // Promote the global to an integer/boolean scalar
	    var_t s = m_lfac.mkArraySingletonVar(r); 
	    if (isInteger(I)) {
	      m_bb.assign(lhs->getVar(), s);
	    } else if (isBool(I)) {  
	      m_bb.bool_assign(lhs->getVar(), s, false);
	    } else { /* unreachable */ }
	  } else {
	    var_t t = lhs->getVar();
	    // Due to heap abstraction imprecisions, it can happen
	    // that the region's bitwidth is smaller than lhs'
	    // bitwidth. 
	    if (r.get_bitwidth() < lhs->getVar().get_bitwidth()) {
	      t = m_lfac.mkIntVar(r.get_bitwidth());
	    }
	    /** 
	     * TODO: We completely forget the array index. This is ok
	     * for array smashing but it will be too imprecise for
	     * other array domains. We need to perform static analysis
	     * to identify for a given pointer its offset wrt to its
	     * allocation site.
	     **/
	    var_t idx = get_unconstrained_array_index_variable(m_lfac.get_vfac()); 
	    auto const* crab_stmt =
	      m_bb.array_load(t, m_lfac.mkArrayVar(r), idx,
			      m_dl->getTypeAllocSize(I.getType()));
	    insert_rev_map(crab_stmt, I);
	    if (r.get_bitwidth() < lhs->getVar().get_bitwidth()) {
	      // XXX: not sure if signed extension is correct.
	      // Regions are signed-agnostic so dont know what is the
	      // best choice here. Maybe if the regions' bitwidth is
	      // different form lhs' bitwidth we should ignore the
	      // load instruction.
	      m_bb.sext(t, lhs->getVar());
	    }
	  }
	  return;
	}
      } else if (isPointer(I, m_params)) {
	if (!lhs || !lhs->isVar()) {
	  CLAM_ERROR("unexpected lhs of load instruction");
	}
	
	m_bb.ptr_load(lhs->getVar(), ptr->getVar());
	return;
      } 
      
      havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
    }
    
    void visitAllocaInst(AllocaInst &I) {

      if (!isTracked(I, m_params)) {
	return;
      }
      
      if (isPointer(I, m_params)) {
	crab_lit_ref_t lhs = m_lfac.getLit(I);
	assert(lhs && lhs->isVar());
	m_bb.ptr_new_object(lhs->getVar(), m_object_id++);
      }

      Function& func = *(I.getParent()->getParent());
      if (m_params.enabled_array_initialization()) {
	mem_region_t r = get_region(m_mem, func, &I);
	if (!r.isUnknown()) {
	  
	  // Nodes which do not have an explicit initialization are
	  // initially undefined. Instead, we assume they are zero
	  // initialized so that Crab's array smashing can infer
	  // something meaningful.
	  
	  Type* elementTy = nullptr;
	  unsigned numElems = 0;
	  if (SequentialType *ST = dyn_cast<SequentialType>(I.getAllocatedType())) {
	    elementTy = ST->getElementType();
	    /* we only translate pointers or arrays */
	    if (isa<PointerType>(ST)) {
	      numElems = 1;
	    } else if (ArrayType *AT = dyn_cast<ArrayType>(ST)) {
	      numElems = AT->getArrayNumElements();
	    } 
	  }

	  if (elementTy && numElems > 0) {
	    if (m_init_regions.insert(r).second) {
	      unsigned elemSize = storageSize(elementTy);
	      if (elemSize > 0) {
		/*
		  XXX: arbitrary value: we choose zero because it has
		  a valid interpretation whether it's integer,
		  boolean or pointer.
		*/
		number_t init_val(0); 
		number_t lb_idx(0); 
		number_t ub_idx((numElems * elemSize) - 1);
		m_bb.array_init(m_lfac.mkArrayVar(r), lb_idx, ub_idx, init_val, elemSize);
	      }
	    }
	  }
	}
      }
    }

    void visitReturnInst(ReturnInst &I) {
      // translated elsewhere      
    }

    void visitCallInst(CallInst &I) {
      CallSite CS(&I);
      const Value *calleeV = CS.getCalledValue();
      const Function *callee =dyn_cast<Function>(calleeV->stripPointerCasts());
      
      if (!callee) {         
	if (I.isInlineAsm()) {
	  // -- inline asm: do nothing 
	} else {
	  // -- unresolved indirect call
	  CLAM_WARNING("skipped indirect call. Enabling --devirt-functions might help.");
	  
	  if (DoesCallSiteReturn(I, m_params) &&
	      ShouldCallSiteReturn(I, m_params)) {
	    // havoc return value
	    crab_lit_ref_t lhs = m_lfac.getLit(I);
	    assert(lhs && lhs->isVar());
	    havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  }
	}
	return;
      }

      // -- ignore any shadow functions created by seahorn
      if (callee->getName().startswith("shadow.mem")) return;

      if (callee->getName().equals("seahorn.fn.enter")) return;

      if (isVerifierCall(callee)) {
        doVerifierCall(I);
        return;
      }
      
      if (isAllocationFn(&I, m_tli)){
        doAllocFn(I);
        return;
      }

      if (m_params.enabled_array_initialization() &&
	  (isZeroInitializer(callee) || isIntInitializer(callee))) {
	doInitializer(I);
	return;
      }
      
      if (callee->isIntrinsic()) {
        if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(&I)) {
          doMemIntrinsic(*MI);
        } else {
	  if (DoesCallSiteReturn(I, m_params) &&
	      ShouldCallSiteReturn(I, m_params)) {
	    // -- havoc return value of the intrinsics
	    crab_lit_ref_t lhs = m_lfac.getLit(I);
	    assert(lhs && lhs->isVar());
	    havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	  }
	}
	return;
      }
      

      if (callee->isDeclaration() || callee->isVarArg() ||
	  !m_params.interprocedural) {
	/**
	 * If external or we don't perform inter-procedural reasoning
	 * then we make sure all modified arrays and return value of
	 * the callsite are havoc'ed.
	 **/      
	
        // -- havoc return value
        if (DoesCallSiteReturn(I, m_params) &&
	    ShouldCallSiteReturn(I, m_params)) {
	  crab_lit_ref_t lhs = m_lfac.getLit(I);
	  assert(lhs && lhs->isVar());
	  havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
	}
        // -- havoc all modified regions by the callee
        if (m_lfac.get_track() == ARR) {
	  mem_region_vector_t mods = get_modified_regions(m_mem, I);
          for (auto a : mods) {
	    if (isGlobalSingleton(a, m_params.lower_singleton_aliases))
	      m_bb.havoc(m_lfac.mkArraySingletonVar(a));
	    else
	      m_bb.havoc(m_lfac.mkArrayVar(a));
          }
        }
	
	// XXX: if we return here we skip the callsite. This is fine
	//      unless there exists an analysis which cares about
	//      external calls.
	//	
	//      Note: if we want to add the callsite make sure we add
	//      the prototype for the external function below.
	// 
        return;
      }

      /**
       * Translate a LLVM callsite 
       *     o := foo(i1,...,i_n) 
       * 
       * into a crab callsite
       *     (o,a_o1,...,a_om) := foo(i1,...,in,a_i1,...,a_in) where
       *  
       *    - a_i1,...,a_in are read-only and modified arrays by foo.
       *    - a_o1,...,a_om are modified and new arrays created inside foo.
       **/

      
      std::vector<var_t> inputs, outputs;
      
      // -- add the actual parameters of the llvm callsite: i1,...in.
      for (auto &a : llvm::make_range(CS.arg_begin(), CS.arg_end())) {
        Value *v = a.get();
        if (!isTracked(*v, m_params)) continue;
        inputs.push_back(normalizeFuncParamOrRet(*v, m_bb, m_lfac));
      }
      
      // -- add the return value of the llvm calliste: o
      if (ShouldCallSiteReturn(I, m_params)) {
	if (DoesCallSiteReturn(I, m_params)) {
	  crab_lit_ref_t ret = m_lfac.getLit(I);
	  assert(ret && ret->isVar());
	  outputs.push_back(ret->getVar());
	} else {
	  // The callsite should return something to match with the
	  // function signature but it doesn't: we create a fresh
	  // return value.
	  Type* RT = callee->getReturnType();
	  if (isBool(RT)) {
	    var_t fresh_ret = m_lfac.mkBoolVar();
	    outputs.push_back(fresh_ret);	    	    
	  } else if (isInteger(RT)) {
	    unsigned bitwidth = RT->getIntegerBitWidth();      
	    var_t fresh_ret = m_lfac.mkIntVar(bitwidth);	    
	    outputs.push_back(fresh_ret);	    	    
	  } else if (isPointer(RT, m_params)) {
	    var_t fresh_ret = m_lfac.mkPtrVar();	    	    
	    outputs.push_back(fresh_ret);	    
	  } else {
	    // do nothing
	  }
	}
      } else {
	if (DoesCallSiteReturn(I, m_params)) {
	  // LLVM shouldn't allow this.
	  CLAM_ERROR("Unexpected type mismatch between callsite and function signature");
	}
      }
      
      if (m_lfac.get_track() == ARR) {
	// -- add the input and output array parameters a_i1,...,a_in
	// -- and a_o1,...,a_om.
        mem_region_vector_t onlyreads = get_read_only_regions(m_mem, I);
        mem_region_vector_t mods = get_modified_regions(m_mem, I);
        mem_region_vector_t news = get_new_regions(m_mem, I);
        
        CRAB_LOG("cfg-mem",
                 llvm::errs() << "Callsite " << I << "\n"
		 << "\tOnly-Read regions " << onlyreads.size() << ": " << onlyreads << "\n"
		 << "\tModified regions " << mods.size() << ": " << mods << "\n"
		 << "\tNew regions " << news.size() << ": " << news << "\n");

        // -- add only read regions as array input parameters
        for (auto a: onlyreads) {
          if (isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
	    inputs.push_back(m_lfac.mkArraySingletonVar(a));
	  } else {
            inputs.push_back(m_lfac.mkArrayVar(a));
	  }
        }
	
        // -- add modified regions as both input and output parameters
        for (auto a: mods) {
	  if (std::find(news.begin(), news.end(), a) != news.end()) {
	    continue;
	  }
	  
          // input version
          if (isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
            inputs.push_back(m_lfac.mkArraySingletonVar(a));  
	  } else {
            inputs.push_back(m_lfac.mkArrayVar(a));
	  }

          // output version
          if (isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
            outputs.push_back(m_lfac.mkArraySingletonVar(a));  	    
	  } else  {
            outputs.push_back(m_lfac.mkArrayVar(a));
	  }
          
        }	
        // -- add more output parameters
        for (auto a: news) {
          outputs.push_back(m_lfac.mkArrayVar(a));
	}
      }
      // -- Finally, add the callsite
      m_bb.callsite(callee->getName().str(), outputs, inputs);
      
    }

    void visitUnreachableInst(UnreachableInst &I) {
      m_bb.unreachable();
    }

    /// base case. if all else fails.
    void visitInstruction(Instruction &I) {
      if (!isTracked(I, m_params)) return;
      CLAM_WARNING("Skipped " << I); 
      crab_lit_ref_t lhs = m_lfac.getLit(I);
      if (lhs && lhs->isVar()) {
	havoc(lhs->getVar(), m_bb, m_params.include_useless_havoc);
      }
    }
    
  }; // end class


  class CfgBuilderImpl {
   public:
    
    CfgBuilderImpl(const llvm::Function& func, 
		   llvm_variable_factory &vfac,
		   HeapAbstraction &mem, 
		   const llvm::TargetLibraryInfo *tli,
		   const CrabBuilderParams &params);
    
    void build_cfg();

    CfgBuilderImpl(const CfgBuilderImpl& o) = delete;
    
    CfgBuilderImpl& operator=(const CfgBuilderImpl& o) = delete;
    
    ~CfgBuilderImpl();

    // return crab control flow graph
    cfg_t& get_cfg();

    // map a llvm basic block to a crab basic block label
    basic_block_label_t get_crab_basic_block(const llvm::BasicBlock *bb) const;

    // map a llvm edge to a crab basic block label.
    // return nullptr if the edge is not translated to a crab basic block.
    const basic_block_label_t*
    get_crab_basic_block(const llvm::BasicBlock *src,
  			 const llvm::BasicBlock *dst) const;

    // Most crab statements have back pointers to LLVM operands so it
    // is always possible to find the corresponding LLVM
    // instruction. Array crab operations are an exceptions.
    // 
    // This method maps an **array** crab statement to its
    // corresponding llvm instruction. Return null if the the array
    // instruction is not mapped to a LLVM instruction.
    const llvm::Instruction* get_instruction(const statement_t& s) const;

   private:

    // map from a llvm basic block to a crab basic block id
    using node_to_crab_block_map_t = std::unordered_map<const llvm::BasicBlock*,
  						      basic_block_label_t>;

    // map from a llvm edge to a crab basic block id
    using edge_to_crab_block_map_t = std::unordered_map<std::pair<const llvm::BasicBlock*,
  								  const llvm::BasicBlock*>,
  							basic_block_label_t, pair_hash>;
    
    // keep track whether the crab CFG has been built
    bool m_is_cfg_built;
    // The function should be const because it's never modified.
    llvm::Function &m_func;
    // literal factory
    crabLitFactory m_lfac; 
    HeapAbstraction &m_mem;
    // the crab CFG
    std::unique_ptr<cfg_t> m_cfg;
    // generate unique identifiers for crab basic block ids
    unsigned int m_id;
    // map llvm CFG basic blocks to crab basic block ids
    node_to_crab_block_map_t m_node_to_crab_map;
    // map llvm CFG edges to crab basic block ids
    edge_to_crab_block_map_t m_edge_to_crab_map;
    // map Crab statement to its corresponding LLVM instruction
    // 
    // In most of the crab statements, their operands have back
    // pointers to their corresponding LLVM values. However, this is
    // not the case for array instructions. For those case, we keep
    // explicitly the reverse mapping.
    llvm::DenseMap<const statement_t*, const llvm::Instruction*> m_rev_map;
    // information about LLVM pointers
    const llvm::DataLayout* m_dl;
    const llvm::TargetLibraryInfo *m_tli;
    // cfg builder parameters
    const CrabBuilderParams& m_params;
    
    /// Helpers for build_cfg
    
    // Given a llvm basic block return its corresponding crab basic block    
    basic_block_t* lookup(const llvm::BasicBlock &bb) const;

    void add_block(const llvm::BasicBlock &bb);
    
    void add_edge(const llvm::BasicBlock &src, const llvm::BasicBlock &target);

    basic_block_t* exec_edge(const llvm::BasicBlock &src,
  			     const llvm::BasicBlock &target); 

    void add_block_in_between(basic_block_t &src, basic_block_t &dst,
  			      basic_block_t &between);
    
    basic_block_label_t make_crab_basic_block_label(const llvm::BasicBlock* bb);
    
    basic_block_label_t make_crab_basic_block_label(const llvm::BasicBlock* src,
  						    const llvm::BasicBlock* dst);
  }; // end class CfgBuilderImpl

  
  CfgBuilderImpl::CfgBuilderImpl(const Function& func, 
				 llvm_variable_factory &vfac,
				 HeapAbstraction &mem, 
				 const TargetLibraryInfo* tli,
				 const CrabBuilderParams &params) 
    : m_is_cfg_built(false),
      // HACK: it's safe to remove constness because we know that the
      // Builder never modifies the bitcode.
      m_func(const_cast<Function&>(func)),
      m_lfac(vfac, params),
      m_mem(mem),
      m_cfg(nullptr),
      m_id(0),             
      m_dl(&(func.getParent()->getDataLayout())),
      m_tli(tli),
      m_params(params) {
    m_cfg.reset(new cfg_t(make_crab_basic_block_label(&m_func.getEntryBlock()),
			  m_params.precision_level));
  }

  CfgBuilderImpl::~CfgBuilderImpl() {}

  cfg_t& CfgBuilderImpl::get_cfg() {
    // it won't build if already built
    build_cfg();
    return *m_cfg;
  }

  const llvm::Instruction* CfgBuilderImpl::get_instruction(const statement_t& s) const {
    auto it = m_rev_map.find(&s);
    if (it != m_rev_map.end()){
      return it->second;
    } else {
      return nullptr;
    }
  }
  
  basic_block_label_t CfgBuilderImpl::get_crab_basic_block(const BasicBlock *bb) const {
    auto it = m_node_to_crab_map.find(bb);
    if (it == m_node_to_crab_map.end()) {
      CLAM_ERROR("cannot map llvm basic block ", bb->getName(),
		 " to crab basic block label");
    }
    return it->second;
    
  }
  
  const basic_block_label_t*
  CfgBuilderImpl::get_crab_basic_block(const BasicBlock *src,
				   const BasicBlock *dst) const {
    auto it = m_edge_to_crab_map.find(std::make_pair(src, dst));
    if (it != m_edge_to_crab_map.end()) {
      return &(it->second);
    } else {
      return nullptr;
    }
  }
    
  // Given a llvm basic block return its corresponding crab basic block
  basic_block_t* CfgBuilderImpl::lookup(const BasicBlock &bb) const {
    auto it = m_node_to_crab_map.find(&bb);
    if (it == m_node_to_crab_map.end()) {
      return nullptr;
    }
    return &(m_cfg->get_node(it->second));
  }

  void CfgBuilderImpl::add_block(const BasicBlock &bb) {
    auto it = m_node_to_crab_map.find(&bb);
    if (it == m_node_to_crab_map.end()) {
      auto bb_label = make_crab_basic_block_label(&bb);
      m_cfg->insert(bb_label);
    }
  }  

  void CfgBuilderImpl::add_edge(const BasicBlock &src, const BasicBlock &dst) {
    basic_block_t* crab_src = lookup(src);
    basic_block_t* crab_dst = lookup(dst);
    assert(crab_src && crab_dst);
    *crab_src >> *crab_dst;
  }  

  void CfgBuilderImpl::add_block_in_between(basic_block_t &src, basic_block_t &dst,
					    basic_block_t &bb) {
    src -= dst;
    src >> bb;
    bb >> dst;
  }  

  basic_block_label_t CfgBuilderImpl::make_crab_basic_block_label(const BasicBlock* bb) {
    ++m_id;
    basic_block_label_t res(bb, m_id);
    m_node_to_crab_map.insert({bb, res});
    return res;
  }

  static std::string create_bb_name(unsigned id, std::string prefix = "") {
    if (prefix == "") prefix = std::string("__@bb_");
    std::string id_str = std::to_string(id);
    return prefix + id_str;
  }

  basic_block_label_t CfgBuilderImpl::make_crab_basic_block_label(const BasicBlock* src,
								  const BasicBlock* dst) {
    ++m_id;    
    std::string name = create_bb_name(m_id);
    basic_block_label_t res(src, dst, name, m_id);
    m_edge_to_crab_map.insert({{src, dst}, res});    
    return res;
  }

  //! return the new block inserted between src and dest if any
  basic_block_t* CfgBuilderImpl::exec_edge(const BasicBlock &src, const BasicBlock &dst) {
    if (const BranchInst *br=dyn_cast<const BranchInst>(src.getTerminator())) {
      if (br->isConditional()) {
        basic_block_t* crab_src = lookup(src);
        basic_block_t* crab_dst = lookup(dst);
        assert(crab_src && crab_dst);

	// Create a new crab block that represents the LLVM edge
	auto bb_label = make_crab_basic_block_label(&src, &dst);	
  	basic_block_t &bb = m_cfg->insert(bb_label);
        add_block_in_between(*crab_src, *crab_dst, bb);

	// Populate the new crab block with an assume
        const Value &c = *br->getCondition();
        if (const ConstantInt *ci = dyn_cast<const ConstantInt>(&c)) {
          if ((ci->isOne()  && br->getSuccessor(0) != &dst) ||
              (ci->isZero() && br->getSuccessor(1) != &dst)) {
            bb.unreachable();
	  }
        } else if (const ConstantExpr *ce = dyn_cast<const ConstantExpr>(&c)) {
	  CLAM_WARNING("Clam cfg builder skipped a branch condition with constant expression");
	} else {
          bool isNegated = (br->getSuccessor(1) == &dst);
	  bool lower_cond_as_bool = false;
          if (CmpInst* CI = dyn_cast<CmpInst>(& const_cast<Value&>(c)))  {
	    if (isBool(*(CI->getOperand(0))) && isBool(*(CI->getOperand(1)))) {
	      lower_cond_as_bool = true;
	    } else if (isInteger(*(CI->getOperand(0))) && isInteger(*(CI->getOperand(1)))) {
	      auto cst_opt = cmpInstToCrabInt(*CI, m_lfac, isNegated);
	      if (cst_opt.hasValue()) {
		bb.assume(cst_opt.getValue()); 
	      }
	    } else if (isPointer(*(CI->getOperand(0)), m_params) &&
		       isPointer(*(CI->getOperand(1)), m_params)) {
	      auto cst_opt = cmpInstToCrabPtr(*CI, m_lfac, isNegated);
	      if (cst_opt.hasValue()) {
		bb.ptr_assume(cst_opt.getValue()); 
	      }
	    }
	    if (c.hasNUsesOrMore(2)) {
	      // If I is used by another instruction apart from a
	      // branch condition.
	      lower_cond_as_bool = true;
	    }
          } else  {
            // If the boolean condition is passed directly (e.g.,
            // after optimization) as a function argument.
	    lower_cond_as_bool = true;
	  }

	  if (lower_cond_as_bool) {
	    crab_lit_ref_t lhs = m_lfac.getLit(c);
	    assert(lhs && lhs->isVar());
	    assert(lhs->isBool());
	    if (isNegated)
	      bb.bool_not_assume(lhs->getVar());
	    else
	      bb.bool_assume(lhs->getVar());
	  }
        }
        return &bb;
      } else {
	// br is unconditional
	add_edge(src,dst);
      }
    } else  if (const SwitchInst *SI = dyn_cast<SwitchInst>(src.getTerminator())) {
      // switch <value>, label <defaultdest> [ <val>, label <dest> ... ]
      //
      // TODO: we do not translate precisely switch instructions. We
      // simply add an edge from src to dest.

      // To be precise, we need to create a block between src and dest
      // and add the statement "assume(value == val)" if dest is not
      // the default block. For the default block, we need to add the
      // sequence:
      //      "assume(value != val1); ... ; assume(value != valk);"

      add_edge(src,dst);
    }
    return nullptr;    
  }

  static bool checkAllDefinitionsHaveNames(const Function& F) {
    // for (auto &Arg : F.args()) {
    //   if (!Arg.hasName()) {
    // 	return false;
    //   }
    // }
    
    for (const BasicBlock &BB: F) {
      if(!BB.hasName()) {
	return false;
      }
      for (const Instruction &I: BB) {
        if (!I.hasName() && !(I.getType()->isVoidTy()))  {
	  return false;
	}
      }
    }
    return true;
  }

  void CfgBuilderImpl::build_cfg() {
    if (m_is_cfg_built) {
      return;
    }
    m_is_cfg_built = true;
    crab::ScopedCrabStats __st__("CFG Construction");

    // Sanity check: pass NameValues must have been executed before
    bool res = checkAllDefinitionsHaveNames(m_func);
    if (!res) {
      CLAM_ERROR("All blocks and definitions must have a name");
    }

    // Create create basic block for each LLVM block
    for (auto &B : m_func) { 
      add_block(B); 
    }

    basic_block_t *ret_block = nullptr;
    var_ref_t ret_val;    
    bool has_seahorn_fail = false;
    // keep track of initialized regions
    std::set<mem_region_t> init_regions;
    
    for (auto &B : m_func) {
      basic_block_t* bb = lookup(B);
      if (!bb) continue;

      
      // -- build a CFG block ignoring branches, phi-nodes, and return
      CrabInstVisitor v(m_lfac, m_mem, m_dl, m_tli, *bb,
			m_rev_map, init_regions, m_params);
      v.visit(B);
      // hook for seahorn
      has_seahorn_fail |= (v.has_seahorn_fail() && m_func.getName().equals("main"));
      
      // -- process the exit block of the function and its returned value.
      if (ReturnInst *RI = dyn_cast<ReturnInst>(B.getTerminator())) {
	if (ret_block) {
	  //UnifyFunctionExitNodes ensures *at most* one return
	  //instruction per function.
	  CLAM_ERROR("UnifyFunctionExitNodes pass should be run first");
	}
	
        ret_block = bb;
	m_cfg->set_exit(ret_block->label());
	if (has_seahorn_fail) {
	  ret_block->assertion(lin_cst_t::get_false(), getDebugLoc(RI));	  
	}
	if (m_params.interprocedural) {
	  if (Value * RV = RI->getReturnValue()) {
	    if (isTracked(*RV, m_params)) {
	      ret_val = var_ref_t(normalizeFuncParamOrRet(*RV, *ret_block, m_lfac));
	      bb->ret(ret_val.get());
	    }
	  } 
	}
      } else {
	std::vector<const BasicBlock*> succs_vector(succs(B).begin(), succs(B).end());
	// The default destination of a switch instruction does not
	// count as a successor but we want to consider it as a such.
	if (SwitchInst* SI = dyn_cast<SwitchInst>(B.getTerminator())) {
	  succs_vector.push_back(SI->getDefaultDest());
	}
        for (const BasicBlock *dst : succs_vector) {
          // -- move branch condition in bb to a new block inserted
          //    between bb and dst
          basic_block_t* mid_bb = exec_edge(B, *dst);

          // -- phi nodes in dst are translated into assignments in
          //    the predecessor
          CrabPhiVisitor v(m_lfac, m_mem,(mid_bb ? *mid_bb : *bb), B);
          v.visit(const_cast<BasicBlock &>(*dst));
        }
      }
    }


    /**
     ** TODO: add an array init statement for each new region.
     ** This is not sound in general so we need to be careful.
     **/
    /// Allocate arrays with initial values 
    // if (m_params.enabled_aggressive_array_initialization()) {
    //   // getNewRegions returns all the new nodes created by the
    //   // function (via malloc-like functions) except if the function
    //   // is main.
    //   // basic_block_t & entry = m_cfg->get_node(m_cfg->entry());
    //   // mem_region_vector_t news =  get_new_regions(m_mem, m_func);
    //   // for (auto n: news) {
    //   // 	entry.set_insert_point_front();
    //   // }
    // }

    /// Add function declaration
    if (m_params.interprocedural && !m_func.isVarArg()) {
      
      /**
       * Translate LLVM function declaration 
       *   o_ty foo (i1,...,in)
       * 
       * into a crab function declaration
       *
       *   o, a_o1,...,a_om foo (i1,...,in,a_i1,...,a_in) where
       *
       *   - o is the **returned value** of the function (translation
       *     ensures there is always one return instruction and the
       *     returned value is a variable, i.e., cannot be a
       *     constant).
       *
       *   - a_i1,...,a_in are read-only and modified arrays in function foo
       * 
       *   - a_o1,....,a_om are modified and new arrays created inside
       *     foo.
       *
       * It ensures that the set {a_i1,...,a_in} is disjoint from
       * {a_o1,....,a_om}, otherwise crab will complain.
       **/
      
      std::vector<var_t> inputs, outputs;

      basic_block_t & entry = m_cfg->get_node(m_cfg->entry());
      
      if (ret_val.is_null()) {
	// special case: function that do not return but in its
	// signature it has a return type. E.g., "int foo() {
	// unreachable; }"
	const Type& RT = *m_func.getReturnType();
	if (isTrackedType(RT, m_params)) {
	  if (isBool(&RT)) {
	    ret_val = var_ref_t(m_lfac.mkBoolVar());  
	  } else if (isInteger(&RT)) {
	    unsigned bitwidth = RT.getIntegerBitWidth();      
	    ret_val = var_ref_t(m_lfac.mkIntVar(bitwidth));
	  } else {
	    assert(RT.isPointerTy());
	    ret_val = var_ref_t(m_lfac.mkPtrVar());  
	  }
	}
      }

      // -- add the returned value of the llvm function: o
      if (!ret_val.is_null()) {
	outputs.push_back(ret_val.get());
      }
      
      // -- add input parameters i1,...,in
      for (Value &arg : llvm::make_range(m_func.arg_begin(), m_func.arg_end())) {
        if (!isTracked(arg, m_params)) continue;

	crab_lit_ref_t i = m_lfac.getLit(arg);
	assert(i && i->isVar());
	if (!ret_val.is_null() && i->getVar() == ret_val.get()) {
	  // rename i to avoid having same name as output (crab requirement)
	  if (i->isBool()) {
	    var_t fresh_i = m_lfac.mkBoolVar();
	    entry.bool_assign(fresh_i, i->getVar());
	    inputs.push_back(fresh_i);	    	    
	  } else if (i->isInt()) {
	    unsigned bitwidth = arg.getType()->getIntegerBitWidth();      
	    var_t fresh_i = m_lfac.mkIntVar(bitwidth);	    
	    entry.assign(fresh_i, i->getVar());
	    inputs.push_back(fresh_i);	    	    
	  } else if (i->isPtr()) {
	    var_t fresh_i = m_lfac.mkPtrVar();	    	    
	    entry.ptr_assign(fresh_i, i->getVar(), number_t(0));
	    inputs.push_back(fresh_i);	    
	  } else {
	    CLAM_ERROR("unexpected function parameter type");
	  }
	} else {
	  inputs.push_back(i->getVar());
	}
      }

	
      if (m_lfac.get_track() == ARR && (!m_func.getName().equals("main"))) {
        // -- add the input and output array parameters 
        mem_region_vector_t onlyreads = get_read_only_regions(m_mem, m_func);
        mem_region_vector_t mods = get_modified_regions(m_mem, m_func);
        mem_region_vector_t news = get_new_regions(m_mem, m_func);

        CRAB_LOG("cfg-mem",
                 llvm::errs() << "Function " << m_func.getName() 
		 << "\n\tOnly-Read regions " << onlyreads.size() << ": " << onlyreads
		 << "\n\tModified regions " << mods.size() << ": " << mods
		 << "\n\tNew regions " << news.size() << ": " << news << "\n");
		 
        // -- add only read regions as input parameters
        for (auto a: onlyreads) {
          if (isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
	    inputs.push_back(m_lfac.mkArraySingletonVar(a));
	  } else {
            inputs.push_back(m_lfac.mkArrayVar(a));	    
	  }
        }

        // -- add input/output parameters
        for (auto a: mods) {
	  if (std::find(news.begin(), news.end(), a) != news.end()) {
	    continue;
	  }
	  var_ref_t a_in;
	  
          // -- for each parameter `a` we create a fresh version
          //    `a_in` where `a_in` acts as the input version of the
          //    parameter and `a` is the output version. Note that the
          //    translation of the function will not produce new
          //    versions of `a` since all array stores overwrite `a`.

	  /** Added in the entry block of the function **/
          entry.set_insert_point_front();
          if (const Value *v = isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
	    Type* ty = cast<PointerType>(v->getType())->getElementType();
	    var_t s = m_lfac.mkArraySingletonVar(a);
	    if (isInteger(ty)) {
	      a_in = var_ref_t(m_lfac.mkIntVar(ty->getIntegerBitWidth()));
	      entry.assign(s, a_in.get());
	    } else if (isBool(ty)) {
	      a_in = var_ref_t(m_lfac.mkBoolVar());
	      entry.bool_assign(s, a_in.get(), false);	      
	    } else { /* unreachable */ }
	  } else {
	    switch (a.get_type()){
	    case INT_REGION:
	      a_in = var_ref_t(m_lfac.mkIntArrayVar(0 /*unknown bitwidth*/));
	      break;
	    case BOOL_REGION:
	      a_in = var_ref_t(m_lfac.mkBoolArrayVar());
	      break;
	    default: /*unreachable*/ ;;
	    }
	    if (!a_in.is_null())
	      entry.array_assign(m_lfac.mkArrayVar(a), a_in.get());
	  }

          // input version
	  if (!a_in.is_null())
	    inputs.push_back(a_in.get());
	  
          // output version
          if (isGlobalSingleton(a, m_params.lower_singleton_aliases)) {
	    // Promote the global to a scalar
	    outputs.push_back(m_lfac.mkArraySingletonVar(a));
	  } else {
            outputs.push_back(m_lfac.mkArrayVar(a));
	  }
        }

        // -- add more output parameters
        for (auto a: news) {
          outputs.push_back(m_lfac.mkArrayVar(a));
        }
        
      }

      // -- Finally, we add the function declaration

      // Sanity check
      std::vector<var_t> sorted_ins(inputs.begin(), inputs.end());
      std::vector<var_t> sorted_outs(outputs.begin(), outputs.end());
      std::sort(sorted_ins.begin(), sorted_ins.end());
      std::sort(sorted_outs.begin(), sorted_outs.end());
      std::vector<var_t> intersect;
      std::set_intersection(sorted_ins.begin(), sorted_ins.end(),
			    sorted_outs.begin(), sorted_outs.end(),
			    std::back_inserter(intersect));
      if (!intersect.empty()) {
	crab::errs() << "INPUTS: {";
	for (auto i: inputs) { crab::outs() << i << ";"; }
	crab::errs() << "}\n";
	crab::errs() << "OUTPUTS: {";
	for (auto o: outputs) { crab::outs() << o << ";"; }
	crab::errs() << "}\n";
	CLAM_ERROR("function inputs and outputs should not intersect");
      }

      typedef function_decl<number_t, varname_t> function_decl_t;
      m_cfg->set_func_decl(function_decl_t(m_func.getName().str(), inputs, outputs));
    }

    if (m_cfg->has_exit()) {
      // -- Connect all sink blocks with an unreachable instruction to
      //    the exit block.  For a forward analysis this doesn't have
      //    any impact since unreachable becomes bottom anyway.
      //    However, a backward analysis starting with an invariant that
      //    says the exit is unreachable may incorrectly infer that the
      //    preconditions of the error states is false just because it
      //    never propagates backwards from these special sink blocks.      
      basic_block_t &exit = m_cfg->get_node(m_cfg->exit());
      for (auto &B: m_func) {
	if (basic_block_t* b = lookup(B)) {
	  if (b->label() == m_cfg->exit())
	    continue;
	  
	  auto it_pair = b->next_blocks();
	  if (it_pair.first == it_pair.second) {
	    // block has no successors and it is not the exit block
	    for (auto &I: B) 
	      if (isa<UnreachableInst>(I))
		*b >> exit;
	  }
	}
      }
    } else {
      // We did not find an exit block yet:

      // (1) search for this pattern:
      //   entry: goto loop;
      //    loop: goto loop;
      BasicBlock& entry = m_func.getEntryBlock();
      auto entry_next = succs(entry);
      if (std::distance(entry_next.begin(), entry_next.end()) == 1) {
	const BasicBlock* succ = *(entry_next.begin());
	auto succ_next = succs(*succ);
	if (std::distance(succ_next.begin(), succ_next.end()) == 1) {
	  if ((*(succ_next.begin())) == succ) {
	    if (basic_block_t* exit = lookup(*succ)) {
	      m_cfg->set_exit(exit->label());
	    }
	  }
	}
      }

      if (!m_cfg->has_exit()) {
	// (2) We check if there is a block with an unreachable
	// instruction. The pass UnifyFunctionExitNodes ensures that
	// there is at most one unreachable instruction.
	for (auto &B: m_func) {
	  for (auto &I: B) {
	    if (isa<UnreachableInst>(I)) {
	      if (basic_block_t* b = lookup(B)) {
		m_cfg->set_exit(b->label());
		break;
	      }
	    }
	  }
	  if (m_cfg->has_exit()) {
	    break;
	  }
	}
      }

      if (!m_cfg->has_exit()) {
	// (3) Search for the first block without successors.
	for (auto &B: m_func) {
	  if (basic_block_t* b = lookup(B)) {
	    auto it_pair = b->next_blocks();
	    if (it_pair.first == it_pair.second) {
	      m_cfg->set_exit(b->label());
	    }
	  }
	}
      }
    }
    
    if (m_params.simplify) {
      // -- Remove dead statements generated by our translation
      CRAB_VERBOSE_IF(1,crab::get_msg_stream() << "Started CFG dead code elimination\n";); 
      cfg_ref_t cfg_ref(*m_cfg);
      crab::transforms::dead_code_elimination<cfg_ref_t> dce;      
      dce.run(cfg_ref);
      CRAB_VERBOSE_IF(1,crab::get_msg_stream() << "Finished CFG dead code elimination\n";);
		      
      // -- Remove empty blocks after dce
      CRAB_VERBOSE_IF(1, crab::get_msg_stream() << "Started CFG simplification\n";);
      m_cfg->simplify();      
      CRAB_VERBOSE_IF(1, crab::get_msg_stream() << "Finished CFG simplification\n";);
    }

    if (m_params.print_cfg) {
      crab::outs () << *m_cfg << "\n";
    }
    return ;
  }


  /* CrabBuilderParams class */

  void CrabBuilderParams::write(raw_ostream &o) const {
    o << "CFG builder options:\n";
    o << "\tabstraction level: ";
    switch(precision_level) {
    case crab::cfg::PTR:
      if (!ignore_ptr) {
	o << "integers and pointers\n";
	break;
      }
    case crab::cfg::NUM:
      o << "only integers\n";
      break;
    case crab::cfg::ARR:
      if (ignore_ptr) {
	o << "integers and arrays (memory abstraction)\n"; 
      } else {
	o << "integers, pointers, and arrays (memory abstraction)\n"; 
      }
      break;
    default:;;
    }
    o << "\tsimplify cfg: " << simplify << "\n";
    o << "\tinterproc cfg: " << interprocedural << "\n";
    o << "\tlower singleton aliases into scalars: " << lower_singleton_aliases << "\n";
    o << "\tinitialize arrays: " << enabled_array_initialization() << "\n";
    o << "\tenable possibly unsound initialization of arrays: "
      << aggressive_initialize_arrays << "\n";
    o << "\tenable big numbers: " << enable_bignums << "\n";
  }
  
  /* CFG Builder class */
  CfgBuilder::CfgBuilder(const llvm::Function& func, CrabBuilderManager &man)
    : m_impl(new CfgBuilderImpl(func,
				man.get_var_factory(),
				man.get_heap_abstraction(),
				&(man.get_tli()),
				man.get_cfg_builder_params())) {}
    
  CfgBuilder::~CfgBuilder() {}
  
  void CfgBuilder::build_cfg() {
    m_impl->build_cfg();
  }
  
  cfg_t& CfgBuilder::get_cfg() {
    return m_impl->get_cfg();
  }

  basic_block_label_t CfgBuilder::
  get_crab_basic_block(const llvm::BasicBlock *bb) const {
    return m_impl->get_crab_basic_block(bb);
  }

  const basic_block_label_t*
  CfgBuilder::get_crab_basic_block(const llvm::BasicBlock *src,
				   const llvm::BasicBlock *dst) const {
    return m_impl->get_crab_basic_block(src, dst);
  }

  const llvm::Instruction* CfgBuilder::get_instruction(const statement_t& s) const {
    return m_impl->get_instruction(s);
  }
  
  /* CFG Manager class */
  CrabBuilderManager::CrabBuilderManager(CrabBuilderParams params,
					 const llvm::TargetLibraryInfo *tli,
					 std::unique_ptr<HeapAbstraction> mem)
    : m_params(params), m_tli(tli), m_mem(std::move(mem)) {}
  
  CrabBuilderManager::~CrabBuilderManager(){}

  CrabBuilderManager::CfgBuilderPtr CrabBuilderManager::mk_cfg_builder(const Function &f) {
    auto it = m_cfg_builder_map.find(&f);
    if (it == m_cfg_builder_map.end()) {
      CfgBuilderPtr builder(new CfgBuilder(f, *this));
      builder->build_cfg();
      m_cfg_builder_map.insert({&f, builder});
      return builder;
    } else {
      return it->second;
    }
  }
					  
  bool CrabBuilderManager::has_cfg(const Function &f) const {
    return m_cfg_builder_map.find(&f) != m_cfg_builder_map.end();
  }
    
  cfg_t& CrabBuilderManager::get_cfg(const Function &f) const {
    return get_cfg_builder(f)->get_cfg();
  }

  CrabBuilderManager::CfgBuilderPtr
  CrabBuilderManager::get_cfg_builder(const Function &f) const {
    auto it = m_cfg_builder_map.find(&f);
    if (it == m_cfg_builder_map.end()) {
      CLAM_ERROR("Cannot find crab cfg for ", f.getName());
    }
    return it->second;
  }

  variable_factory_t& CrabBuilderManager::get_var_factory() {
    return m_vfac;
  }
 
  const CrabBuilderParams& CrabBuilderManager::get_cfg_builder_params() const {
    return m_params;
  }

  const llvm::TargetLibraryInfo& CrabBuilderManager::get_tli() const {
    return *m_tli;
  }
  
  HeapAbstraction& CrabBuilderManager::get_heap_abstraction() {
    return *m_mem;
  }
  
} // end namespace clam
