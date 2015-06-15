
/* Translate a LLVM function to a custom CFG. */

#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Pass.h"

#include <queue>
#include <boost/lexical_cast.hpp>

#include "ikos_llvm/CfgBuilder.hh"
#include "ikos_llvm/Support/CFG.hh"
#include "ikos_llvm/Support/bignums.hh"

//TODO: Translation of pointer arithmetic
//TODO: Translation of calls

namespace llvm_ikos
{

  using namespace cfg_impl;
  using namespace llvm;
  using namespace std;

  /** Converts v to mpz_class. Assumes that v is signed */
  inline mpz_class toMpz (const APInt &v)
  {
    // Based on:
    // https://llvm.org/svn/llvm-project/polly/trunk/lib/Support/GICHelper.cpp
    // return v.getSExtValue ();
    
    APInt abs;
    abs = v.isNegative () ? v.abs () : v;
    
    const uint64_t *rawdata = abs.getRawData ();
    unsigned numWords = abs.getNumWords ();
    
    // TODO: Check if this is true for all platforms.
    mpz_class res;
    mpz_import(res.get_mpz_t (), numWords, 1, sizeof (uint64_t), 0, 0, rawdata);
    
    return v.isNegative () ? mpz_class(-res) : res;
  }

  struct SymExecBase {

    /// -- variable factory
    VariableFactory& m_vfac;
    /// -- current read memory
    Value* m_inMem;
    /// -- current write memory
    Value* m_outMem;
    /// -- if m_inMem is a singleton
    bool   is_inMem_singleton;
    /// -- level of precision of the Cfg
    TrackedPrecision m_track_level;

    SymExecBase (VariableFactory &vfac, TrackedPrecision level): 
        m_vfac (vfac), m_inMem (0), m_outMem (0), is_inMem_singleton (false), 
        m_track_level (level) { }

    bool isShadowMem (const Value &V)
    {
      // work list
      std::queue<const Value*> wl;
      
      wl.push (&V);
      while (!wl.empty ())
      {
        const Value *val = wl.front ();
        wl.pop ();
        
        if (const CallInst *ci = dyn_cast<const CallInst> (val))
        {
          if (const Function *fn = ci->getCalledFunction ())
          return fn->getName ().startswith ("shadow.mem.");
          return false;
        }   
        else if (const PHINode *phi = dyn_cast<const PHINode> (val))
        {
          for (unsigned i = 0; i < phi->getNumIncomingValues (); ++i)
            wl.push (phi->getIncomingValue (i));
        }
        else return false;
      }
      assert (0);
      return false;
    }

    bool isTracked (const llvm::Value &v)
    {
      // -- shadow values represent memory regions
      // -- only track them when memory is tracked
      if (isShadowMem (v)) return m_track_level >= MEM;

      // -- a pointer
      if (v.getType ()->isPointerTy ()) return m_track_level >= PTR;

      // -- always track integer registers
      return v.getType ()->isIntegerTy ();
    }

    varname_t symVar (const Value &v)
    {
      assert (isTracked (v));
      return m_vfac [v];
    }
  
    boost::optional<ZLinearExpression> lookup (const Value &v)
    {
      if (isa<ConstantPointerNull> (&v) || isa<const UndefValue> (&v))
        return boost::optional<ZLinearExpression>();
      
      if (const ConstantInt *c = dyn_cast<const ConstantInt> (&v))
      {
        if (c->getValue ().getMinSignedBits () > 64)
        {
          errs () << "Warning: " << toMpz (c->getValue ()).get_str ()  
                  << " too big for int64_t.\n";
        }
        else
          return boost::optional<ZLinearExpression>
              (ZLinearExpression (c->getValue ().getSExtValue ()));
      }

      if (isTracked(v))
        return boost::optional<ZLinearExpression>(ZLinearExpression (symVar (v)));
      
      return boost::optional<ZLinearExpression>();
    }
    
    bool isLogicalOp(const Instruction &I) const 
    {
      return (I.getOpcode() >= Instruction::And && 
              I.getOpcode() <= Instruction::Xor);
    }
    
    void normalizeCmpInst(CmpInst &I)
    {
      switch (I.getPredicate()){
        case ICmpInst::ICMP_UGT:	
        case ICmpInst::ICMP_SGT: I.swapOperands(); break; 
        case ICmpInst::ICMP_UGE:	
        case ICmpInst::ICMP_SGE: I.swapOperands(); break; 
        default: ;
      }
    }

    boost::optional<ZLinearConstraint> 
    gen_assertion (CmpInst &I, bool is_negated)
    {
      
      normalizeCmpInst(I);
      
      const Value& v0 = *I.getOperand (0);
      const Value& v1 = *I.getOperand (1);

      boost::optional<ZLinearExpression> op1 = lookup (v0);
      boost::optional<ZLinearExpression> op2 = lookup (v1);
      
      ZLinearConstraint res;
      
      if (op1 && op2) 
      {
        switch (I.getPredicate ())
        {
          case CmpInst::ICMP_EQ:
            ( (!is_negated) ? 
              res = ZLinearConstraint (*op1 == *op2) : 
              res = ZLinearConstraint (*op1 != *op2));
            break;
          case CmpInst::ICMP_NE:
            ( (!is_negated) ? 
              res = ZLinearConstraint (*op1 != *op2) : 
              res = ZLinearConstraint (*op1 == *op2));
            break;
          case CmpInst::ICMP_ULT:
          case CmpInst::ICMP_SLT:
            ( (!is_negated) ? 
              res = ZLinearConstraint (*op1 <= *op2 - 1) : 
              res = ZLinearConstraint (*op1 >= *op2));
            break; 
          case CmpInst::ICMP_ULE:
          case CmpInst::ICMP_SLE:
            ( (!is_negated) ? 
              res = ZLinearConstraint (*op1 <= *op2) : 
              res = ZLinearConstraint (*op1 >= *op2 + 1));
            break;
          default:  assert (false);
        }
        return boost::optional<ZLinearConstraint> (res);
      }
      else
        return boost::optional<ZLinearConstraint> ();
    }
  };

  struct SymExecVisitor : public InstVisitor<SymExecVisitor>, SymExecBase
  {
    basic_block_t& m_bb;
    
    SymExecVisitor (VariableFactory& vfac, basic_block_t & bb, TrackedPrecision level): 
        SymExecBase (vfac, level), m_bb(bb)  {  }
        
    /// skip PHI nodes
    void visitPHINode (PHINode &I) {}
    
    void visitCmpInst (CmpInst &I) 
    {
    
      if (!isTracked (I)) return;
    
      varname_t lhs = symVar(I);

      bool covered = true;

      if (const Value *cond = dyn_cast<const Value> (&I))
      {
        Value::const_use_iterator it = cond->use_begin(), et = cond->use_end();
        for (; it!=et ; ++it)
        {
          const Instruction * User = dyn_cast<Instruction>(*it);
          
          if (const BranchInst *br = dyn_cast<const BranchInst> (User))
          { 
            // Comparison can be used as a "filter". We skip it here because
            // it will treated by SymExecCmpInstVisitor.
            if (br->isConditional () && br->getCondition() == cond) 
              continue;
          }    
          
          if (isa<const SelectInst> (User) ) 
          { // This case is (partially) covered by SymExecITEVisitor
            continue;
          }
          
          if (isa<const CastInst> (User) || isLogicalOp (*User)) 
          {
            // abstraction: source of imprecision
          }
          covered = false;
          break;
        }
      }
      if (!covered)
        m_bb.havoc(lhs);
    }    

    /// skip BranchInst
    void visitBranchInst (BranchInst &I) {}
    
    /// skip SelectInst
    void visitSelectInst(SelectInst &I) {}
  
    void visitBinaryOperator(BinaryOperator &I)
    {
      if (!isTracked (I)) return;
      
      varname_t lhs = symVar(I);
      
      switch (I.getOpcode ())
      {
        case BinaryOperator::Add:
        case BinaryOperator::Sub:
        case BinaryOperator::Mul:
        case BinaryOperator::UDiv:
        case BinaryOperator::SDiv:
        case BinaryOperator::Shl:
          doArithmetic (lhs, I);
          break;
          // case BinaryOperator::And:
          // case BinaryOperator::Or:
          // case BinaryOperator::Xor:
          // case BinaryOperator::AShr:
          // case BinaryOperator::LShr:
        default:
          m_bb.havoc(lhs);
          break;
      }
    }
    
    void doArithmetic (varname_t lhs, BinaryOperator &i)
    {
      const Value& v1 = *i.getOperand(0);
      const Value& v2 = *i.getOperand(1);
      
      boost::optional<ZLinearExpression> op1 = lookup (v1);
      boost::optional<ZLinearExpression> op2 = lookup (v2);
      
      if (!(op1 && op2)) return;
      
      switch(i.getOpcode())
      {
        case BinaryOperator::Add:
          m_bb.add (lhs, *op1, *op2);
          break;
        case BinaryOperator::Sub:
            if ((*op1).is_constant())            
            { // cfg does not support subtraction of a constant by a
              // variable because the ikos api for abstract domains
              // does not support it.
              m_bb.assign (lhs, ZLinearExpression ((*op1).constant ()));
              m_bb.sub (lhs, ZLinearExpression (lhs), *op2);
            }
            else
              m_bb.sub (lhs, *op1, *op2);
          break;
        case BinaryOperator::Mul:
          m_bb.mul (lhs, *op1, *op2);
          break;
        case BinaryOperator::SDiv:
          {
            if ((*op1).is_constant())            
            { // cfg does not support division of a constant by a
              // variable because the ikos api for abstract domains
              // does not support it.
              m_bb.assign (lhs, ZLinearExpression ((*op1).constant ()));
              m_bb.div (lhs, ZLinearExpression (lhs), *op2);
            }
            else
              m_bb.div (lhs, *op1, *op2);
          }
          break;
        case BinaryOperator::Shl:
          if ((*op2).is_constant())
          {
            ikos::z_number k = (*op2).constant ();
            int shift = boost::lexical_cast<int>(toStr (k));
            assert (shift >= 0);
            unsigned factor = 1;
            for (unsigned i = 0; i < (unsigned) shift; i++) 
              factor *= 2;
            m_bb.mul (lhs, *op1, ZLinearExpression (factor));            
          }
          break;
        default:
          m_bb.havoc(lhs);
          break;
      }
    }
    
    void visitTruncInst(TruncInst &I)              
    { doCast (I); }
    
    void visitZExtInst (ZExtInst &I)
    { doCast (I); }
    
    void visitSExtInst (SExtInst &I)
    { doCast (I); }
    
    void doCast(CastInst &I)
    {
      if (!isTracked (I)) return;

      varname_t dst = symVar (I);
      const Value& v = *I.getOperand (0); // the value to be casted

      boost::optional<ZLinearExpression> src = lookup (v);

      if (src)
        m_bb.assign(dst, *src);
      else
      {
        if (v.getType ()->isIntegerTy (1))
        {
          m_bb.assume ( ZLinearExpression (dst) >= ZLinearExpression (0) );
          m_bb.assume ( ZLinearExpression (dst) <= ZLinearExpression (1) );
        }
        else m_bb.havoc(dst);
      }
    }

    void visitLoadInst (LoadInst &I)
    {
      if (!isTracked (I)) return;

      if (!m_inMem) return;

      varname_t lhs = symVar (I);
      boost::optional<ZLinearExpression> idx = lookup (*I.getPointerOperand ());
      if (!idx) return ;

      m_bb.array_load (lhs, symVar (*m_inMem), *idx);

      m_inMem = NULL;
      is_inMem_singleton = false;
    }
    
    void visitStoreInst (StoreInst &I)
    {
      if (!m_inMem || !m_outMem || !isTracked (*I.getOperand (0))) return;

      boost::optional<ZLinearExpression> idx = lookup (*I.getPointerOperand ());
      if (!idx) return;

      boost::optional<ZLinearExpression> val = lookup (*I.getOperand (0));
      assert (val);

      m_bb.array_store (symVar (*m_outMem), symVar (*m_inMem), *idx, *val, 
                        is_inMem_singleton);      
      
      m_inMem = NULL;
      is_inMem_singleton = false;
      m_outMem = NULL;
    }

    void visitCallInst (CallInst &I) 
    {
      CallSite CS (&I);
      const Function *fn = CS.getCalledFunction ();
      if (!fn) {
        // Use DSA to handle better
        return;      
      }
      
      const Function &F = *fn;
      const Function &PF = *I.getParent ()->getParent ();

      if (F.getName ().startswith ("shadow.mem") && isTracked (I))
      {
        if (F.getName ().equals ("shadow.mem.init"))
        { 
          m_bb.array_init (symVar (I));
        }
        else if (PF.getName ().equals ("main") && 
                 F.getName ().equals ("shadow.mem.arg.init"))
        {
          m_bb.array_init (symVar (I));
        }
        else if (F.getName ().equals ("shadow.mem.load"))
        { 
          m_inMem = CS.getArgument (1);
        }
        else if (F.getName ().equals ("shadow.mem.store"))
        {
          m_inMem = CS.getArgument (1);
          m_outMem = &I;
          is_inMem_singleton = false;
          if (const ConstantInt *c = dyn_cast<const ConstantInt> (CS.getArgument (2)))
          {
            if (c->getType ()->isIntegerTy (1))
            {
              is_inMem_singleton = c->isOne ();
            }
          }
        }
        else if (F.getName ().equals ("shadow.mem.arg.ref"))
        {// TODO: read-only array by the function
          //m_fparams.push_back (m_s.read (symb (*CS.getArgument (1))));
        }
        else if (F.getName ().equals ("shadow.mem.arg.mod"))
        {
          //m_fparams.push_back (m_s.read (symb (*CS.getArgument (1))));
          //m_fparams.push_back (m_s.havoc (symb (I)));
          m_bb.havoc (symVar (I));
        }
        else if (F.getName ().equals ("shadow.mem.arg.new"))
        {// TODO: new array created by the function
          //m_fparams.push_back (m_s.havoc (symb (I)));
        }
        else if (!PF.getName ().equals ("main") && 
                 F.getName ().equals ("shadow.mem.in"))
        {// TODO: input array variables
          //m_s.read (symb (*CS.getArgument (1)));
        }
        else if (!PF.getName ().equals ("main") &&
                 F.getName ().equals ("shadow.mem.out"))
        {// TODO: output array variables
          //m_s.read (symb (*CS.getArgument (1)));
        }
        else if (!PF.getName ().equals ("main") && 
                 F.getName ().equals ("shadow.mem.arg.init"))
        {
          // regions initialized in main are global. 
          // do nothing 
        }
      }
    }

    /// base case. if all else fails.
    void visitInstruction (Instruction &I)
    {
      if (!isTracked (I)) return;
      if (isa<AllocaInst> (I)) return;

      varname_t lhs = symVar(I);
      m_bb.havoc(lhs);
    }
    
  };

  struct SymExecPhiVisitor : public InstVisitor<SymExecPhiVisitor>, SymExecBase
  {
    // block where assignment/havoc will be inserted
    basic_block_t&    m_bb; 
    // incoming block of the PHI instruction
    const llvm::BasicBlock& m_inc_BB; 
    
    SymExecPhiVisitor (VariableFactory& vfac, basic_block_t& bb, 
                       const llvm::BasicBlock& inc_BB, TrackedPrecision level): 
        SymExecBase (vfac,level), m_bb (bb), m_inc_BB (inc_BB)   {}
    
    void visitPHINode (PHINode &I) 
    {
      if (!isTracked (I)) return;
      
      const Value *LHS = dyn_cast<const Value>(&I);
      const Value &v = *I.getIncomingValueForBlock (&m_inc_BB);
      
      if (LHS == &v) return;
      
      varname_t lhs = symVar(I);
      boost::optional<ZLinearExpression> rhs = lookup(v);
      if (rhs) m_bb.assign(lhs, *rhs);
      else     m_bb.havoc(lhs);
    }
  };


  struct SymExecCmpInstVisitor : public InstVisitor<SymExecCmpInstVisitor>, SymExecBase
  {
    basic_block_t& m_bb;
    bool           m_is_negated;
    
    SymExecCmpInstVisitor (VariableFactory& vfac, basic_block_t& bb, 
                           bool is_negated, TrackedPrecision level):
        SymExecBase (vfac,level), m_bb (bb), m_is_negated (is_negated)   {}
    
    void visitCmpInst (CmpInst &I) 
    {
      
      boost::optional<ZLinearConstraint> cst = gen_assertion (I, m_is_negated);
      if (cst) m_bb.assume(*cst);
      
      if (isTracked (I))
      {
        varname_t lhs = symVar (I);
        if (m_is_negated)
          m_bb.assign (lhs, ZLinearExpression (0));
        else
          m_bb.assign (lhs, ZLinearExpression (1));
      }
    }
  };
  
  struct SymExecITEVisitor : public InstVisitor<SymExecITEVisitor>, SymExecBase
  {
    
    CfgBuilder&    m_builder;
    basic_block_t& m_bb;
    unsigned id;
    
    SymExecITEVisitor (VariableFactory& vfac, CfgBuilder& builder, 
                       basic_block_t& bb, TrackedPrecision level):
        SymExecBase (vfac, level), m_builder (builder), m_bb (bb), id (0)   
    { }
    
    void visitCmpInst (CmpInst &I) { }
  
    void visitSelectInst(SelectInst &I)
    {
      
      if (!isTracked (I)) return;
      
      varname_t lhs = symVar(I);
      
      const Value& cond = *I.getCondition ();
      boost::optional<ZLinearExpression> op0 = lookup (*I.getTrueValue ());
      boost::optional<ZLinearExpression> op1 = lookup (*I.getFalseValue ());
      
      if (!(op0 && op1)) return;
      
      if (const ConstantInt *ci = dyn_cast<const ConstantInt> (&cond))
      {
        if (ci->isOne ())
        {
          m_bb.assign (lhs, *op0);
          return;
        }
        else if (ci->isZero ())
        {
          m_bb.assign (lhs, *op1);
          return;
        }
      }
      
      // abstraction: source of imprecision
      m_bb.havoc(lhs);
    }
  };
  
  CfgBuilder::opt_basic_block_t CfgBuilder::lookup (const llvm::BasicBlock &B)  
  {
    llvm::BasicBlock* BB = const_cast<llvm::BasicBlock*> (&B);
    llvm_bb_map_t::iterator it = m_bb_map.find (BB);
    if (it == m_bb_map.end ())
      return CfgBuilder::opt_basic_block_t ();
    else
      return CfgBuilder::opt_basic_block_t (it->second);
  }

  void CfgBuilder::add_block (llvm::BasicBlock &B)
  {
    assert (!lookup(B));
    llvm::BasicBlock *BB = &B;
    basic_block_t &bb = m_cfg.insert ( BB);
    m_bb_map.insert (llvm_bb_map_t::value_type (BB, bb));
  }  

  // basic_block_t& 
  // CfgBuilder::split_block (basic_block_t & bb) 
  // {
  //   static unsigned id = 0;
  //   std::string new_bb_id_str(bb.name().str() + "_split_" + lexical_cast<string>(id));    
  //   basic_block_label_t new_bb_id(new_bb_id_str.str ());
  //   ++id;
  //   assert (m_bb_map.find (new_bb_id) == m_bb_map.end ()); 
  //   basic_block_t &new_bb = m_cfg.insert (new_bb_id);
  //   for (auto succ: bb.next_blocks ()) 
  //     new_bb >> lookup (succ);
  //   bb.reset_next_blocks ();
  //   bb >> new_bb ;
  //   return new_bb;
  // }


   basic_block_t& CfgBuilder::add_block_in_between (basic_block_t &src, 
                                                    basic_block_t &dst,
                                                    basic_block_label_t bb_id)
  {

    assert (m_bb_map.find(bb_id) == m_bb_map.end()); 

    basic_block_t &bb = m_cfg.insert (bb_id);

    src -= dst;
    src >> bb;
    bb >> dst;

    return bb;
  }  


  void CfgBuilder::add_edge (llvm::BasicBlock &S, 
                             const llvm::BasicBlock &D)
  {
    opt_basic_block_t SS = lookup(S);
    opt_basic_block_t DD = lookup(D);
    assert (SS && DD);
    *SS >> *DD;
  }  

  // return the new block inserted between src and dest if any
  CfgBuilder::opt_basic_block_t 
  CfgBuilder::execBr (llvm::BasicBlock &src, const llvm::BasicBlock &dst)
  {
    // the branch condition
    if (const BranchInst *br = dyn_cast<const BranchInst> (src.getTerminator ()))
    {
      if (br->isConditional ())
      {
        opt_basic_block_t Src = lookup(src);
        opt_basic_block_t Dst = lookup(dst);
        assert (Src && Dst);

        // Dummy BasicBlock 
        basic_block_label_t bb_id = llvm::BasicBlock::Create(m_func.getContext (),
                                                             create_bb_name ());
        
        basic_block_t &bb = add_block_in_between (*Src, *Dst, bb_id);
        
        const Value &c = *br->getCondition ();
        if (const ConstantInt *ci = dyn_cast<const ConstantInt> (&c))
        {
          if ((ci->isOne ()  && br->getSuccessor (0) != &dst) ||
              (ci->isZero () && br->getSuccessor (1) != &dst))
            bb.unreachable();
        }
        else 
        {
          SymExecCmpInstVisitor v (m_vfac, bb, br->getSuccessor (1) == &dst, m_track_level);
          if (llvm::Instruction *I = 
              dyn_cast<llvm::Instruction> (& const_cast<llvm::Value&>(c)))
            v.visit (I); 
          else
            errs () << "Warning: cannot generate guard from " << c << "\n";
        }
        return opt_basic_block_t(bb);
      }
      else add_edge (src,dst);
    }
    return opt_basic_block_t();    
  }

  void CfgBuilder::make_cfg()
  {
    //LOG ("ikos-verbose", errs () << "Begin building muzq CFG ... \n");

    for (auto &B : m_func) 
      add_block(B); 

    std::vector<basic_block_t*> rets;

    for (auto &B : m_func)
    {
      const llvm::BasicBlock *bb = &B;
      if (isa<ReturnInst> (B.getTerminator ()))
      {
        opt_basic_block_t BB = lookup (B);
        assert (BB);
        basic_block_t& bb = *BB;
        rets.push_back (&bb);
        SymExecVisitor v (m_vfac, *BB, m_track_level);
        v.visit (B);
      }
      else
      {
        opt_basic_block_t BB = lookup (B);
        assert (BB);

        // build an initial CFG block from bb but ignoring for now
        // branches, ite instructions and phi-nodes
        {
          SymExecVisitor v (m_vfac, *BB, m_track_level);
          v.visit (B);
        }
        
        for (const llvm::BasicBlock *dst : succs (*bb))
        {
          // move branch condition in bb to a new block inserted
          // between bb and dst
          opt_basic_block_t mid_bb = execBr (B, *dst);

          // phi nodes in dst are translated into assignments in the
          // predecessor
          {            
            SymExecPhiVisitor v (m_vfac, (mid_bb ? *mid_bb : *BB), B, m_track_level);
            v.visit (const_cast<llvm::BasicBlock &>(*dst));
          }
        }
        
        {
          SymExecITEVisitor v (m_vfac, *this, *BB, m_track_level);
          v.visit (B);
        }
      }
    }
    
    // unify multiple return blocks

    if (rets.size () == 1) m_cfg.set_exit (rets [0]->label ());
    else if (rets.size () > 1)
    {
      // Dummy BasicBlock 
      basic_block_label_t unified_ret_id = llvm::BasicBlock::Create(m_func.getContext (),
                                                                    create_bb_name ());

      basic_block_t &unified_ret = m_cfg.insert (unified_ret_id);

      for(unsigned int i=0; i<rets.size (); i++)
        *(rets [i]) >> unified_ret;
      m_cfg.set_exit (unified_ret.label ());
    }

    // Important to keep small the cfg
    m_cfg.simplify ();

    //LOG ("ikos-verbose", errs () << "Done muzq CFG. \n");    
    return ;
  }

} // end namespace llvm_ikos
