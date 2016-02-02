/* 

   This pass aims at profiling the complexity of a program for the
   purpose of proving absence of certain kind of errors such as
   out-of-bound accesses, division by zero, use of uninitialized
   variables, etc.

*/

#include "llvm/Pass.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Format.h"
#include "llvm/Target/TargetLibraryInfo.h"

#include <boost/unordered_map.hpp>

using namespace llvm;

#include "llvm/IR/Instruction.def"

namespace crab_llvm {

  struct Counter {
    unsigned int Id;
    const char*Name;
    unsigned int Value;

    Counter (unsigned int id, const char *name): 
        Id (id), Name(name), Value(0) { }

    bool operator<(const Counter&o) const {
      if (int Cmp = std::strcmp(getName(), o.getName()))
        return Cmp < 0;
      // secondary key
      return Id < o.Id;
    }

    unsigned int getValue() const { return Value; }
    const char *getName() const { return Name; }
    void operator++() { Value++; }
    void operator+=(unsigned val) { Value += val; }
  };


  class CanReadUndef {
    
    static bool printDebugLoc (const Instruction *inst, std::string &msg) {
      if (!inst) return false;
      
      const DebugLoc &dloc = inst->getDebugLoc ();
      if (dloc.isUnknown ()) return false;

      unsigned Line = dloc.getLine ();
      unsigned Col = dloc.getCol ();
      std::string File; 
      DIScope Scope (dloc.getScope ());
      if (Scope) File = Scope.getFilename ();
      else File = "unknown file";
      
      msg += "--- File: " + File + "\n"
          + "--- Line: " + std::to_string(Line)  + "\n" 
          + "--- Column: " + std::to_string(Col) + "\n";
      
      return true;
    }
    
    
    bool runOnFunction (Function &F) {
      for (BasicBlock &b : F) {
        // -- go through all the reads
        for (User &u : b) {
          // phi-node
          if (PHINode* phi = dyn_cast<PHINode> (&u)) {
            for (unsigned i = 0; i < phi->getNumIncomingValues (); i++) {
              if (isa<UndefValue> (phi->getIncomingValue (i))) {
                printDebugLoc (dyn_cast<Instruction> (phi), report);
                num_undef++;
              }              
            }            
            continue;
          }
          // -- the normal case
          for (unsigned i = 0; i < u.getNumOperands (); i++) {
            if (isa <UndefValue> (u.getOperand (i))) {
              printDebugLoc (dyn_cast<Instruction> (u.getOperand (i)), report);
              num_undef++;
            }
          }
        }
      }
      return false;
    }
    
    unsigned int num_undef;
    std::string report;
    
   public:
    
    CanReadUndef(): num_undef (0) { }
    
    bool runOnModule(Module &M)  {
      for (Module::iterator FI = M.begin(), E = M.end(); FI != E; ++FI)
        runOnFunction (*FI);
      return false;
    }
    
    void printReport (raw_ostream &O) {
      O << " =========================== \n";
      O << "   Undefined value analysis  \n";
      O << " ============================\n";
      O << num_undef << " Number of possible uses of undefined values\n";
      O << report << "\n";
    }
  };

  class AnalysisProfiler : public ModulePass, public InstVisitor<AnalysisProfiler> {
    friend class InstVisitor<AnalysisProfiler>;

    boost::unordered_map <unsigned int, Counter> counters;
    void incrementCounter (unsigned id, const char* name, unsigned val) {
      auto it = counters.find (id);
      if (it != counters.end ())
        it->second += val;
      else
        counters.insert (std::make_pair (id, Counter (id, name)));
    }

    const DataLayout* DL;
    TargetLibraryInfo* TLI;
    StringSet<> ExtFuncs;

    unsigned int TotalFuncs;
    unsigned int TotalBlocks;
    unsigned int TotalJoins;
    unsigned int TotalInsts;
    unsigned int TotalDirectCalls;
    unsigned int TotalIndirectCalls;
    unsigned int TotalExternalCalls;
    ///
    unsigned int SafeIntDiv;
    unsigned int SafeFPDiv;
    unsigned int UnsafeIntDiv;
    unsigned int UnsafeFPDiv;
    unsigned int DivUnknown;
    /// 
    unsigned int TotalMemAccess;
    unsigned int MemUnknownSize;
    unsigned int MemUnknown;
    unsigned int SafeMemAccess;
    unsigned int UnsafeMemAccess;
    ///
    unsigned int SafeLeftShift;
    unsigned int UnsafeLeftShift;
    unsigned int UnknownLeftShift;

    void visitFunction  (Function &F) { ++TotalFuncs; }
    void visitBasicBlock(BasicBlock &BB) { 
      ++TotalBlocks; 
      if (!BB.getSinglePredecessor ())
        ++TotalJoins;
    }

    void visitCallSite(CallSite &CS) {
      Function* callee = CS.getCalledFunction ();
      if (callee) {
        TotalDirectCalls++;
        if (callee->isDeclaration ()) {
          TotalExternalCalls++;
          ExtFuncs.insert (callee->getName ());
        }
      }
      else
        TotalIndirectCalls++;
    }

    void processPointerOperand (Value* V) {
      // V = globalVariable | alloca | malloc | load | inttoptr | formal param | return |
      //     (getElementPtr (Ptr) | bitcast (Ptr) | phiNode (Ptr) ... (Ptr) )*  
      // We only check for GEP so we are missing many cases
      TotalMemAccess++;

      if (GetElementPtrInst * GEP = dyn_cast<GetElementPtrInst> (V)) {

        // figure out statically the offset of the pointer
        unsigned BitWidth = DL->getPointerTypeSizeInBits(GEP->getType());
        APInt Offset (BitWidth, 0);
        if (GEP->accumulateConstantOffset (*DL, Offset)) {          
          if (Offset.isNegative ()) {
            UnsafeMemAccess++;
            return;
          }

          // figure out statically the size of the memory object
          uint64_t Size = AliasAnalysis::UnknownSize;
          getObjectSize (GEP->getPointerOperand (), Size, DL, TLI, false);
          if (Size != AliasAnalysis::UnknownSize) {
            uint64_t Offset_val = Offset.getLimitedValue ();
            if (Offset_val < Size)
              SafeMemAccess++;
            else 
              UnsafeMemAccess++;
            
            return;
          }
          else {
            MemUnknownSize++;
            return;
          }
        }
      }
      MemUnknown++;
    }


    void visitBinaryOperator (BinaryOperator* BI) {
      if (BI->getOpcode () == BinaryOperator::SDiv || 
          BI->getOpcode () == BinaryOperator::UDiv ||
          BI->getOpcode () == BinaryOperator::SRem ||
          BI->getOpcode () == BinaryOperator::URem ||
          BI->getOpcode () == BinaryOperator::FDiv || 
          BI->getOpcode () == BinaryOperator::FRem) {
        const Value* divisor = BI->getOperand (1);
        if (const ConstantInt *CI = dyn_cast<const ConstantInt> (divisor)) {
          if (CI->isZero ()) UnsafeIntDiv++;
          else SafeIntDiv++;
        }
        else if (const ConstantFP *CFP = dyn_cast<const ConstantFP> (divisor)) {
          if (CFP->isZero ()) UnsafeFPDiv++;
          else SafeFPDiv++;
        }
        else {
          // cannot figure out statically
          DivUnknown++;
        }
      }
      else if (BI->getOpcode () == BinaryOperator::Shl) {
        // Check for oversized shift amounts
        if (const ConstantInt *CI = dyn_cast<const ConstantInt> (BI->getOperand (1))) {
          APInt shift = CI->getValue ();
          if (CI->getType ()->isIntegerTy ()) {
            APInt bitwidth (32, CI->getType ()->getIntegerBitWidth (), true);
            if (shift.slt (bitwidth))
              SafeLeftShift++;
            else
              UnsafeLeftShift++;
          }
          else 
            UnknownLeftShift++;
        }
        else 
          UnknownLeftShift++;
      }
    }

    #define HANDLE_INST(N, OPCODE, CLASS)                    \
    void visit##OPCODE(CLASS &I) {                           \
      incrementCounter (N, #OPCODE, 1);                      \
      ++TotalInsts;                                          \
      if (CallInst* CI = dyn_cast<CallInst>(&I)) {           \
         CallSite CS (CI);                                   \
         visitCallSite (CS);                                 \
      }                                                      \
      else if (InvokeInst* II = dyn_cast<InvokeInst>(&I)) {  \
         CallSite CS (II);                                   \
         visitCallSite (CS);                                 \
      }                                                      \
      else if (BinaryOperator* BI = dyn_cast<BinaryOperator>(&I)) { \
         visitBinaryOperator (BI);                                  \
      }                                                             \
      else if (LoadInst* LI = dyn_cast<LoadInst>(&I)) {             \
         processPointerOperand (LI->getPointerOperand ());          \
      }                                                             \
      else if (StoreInst* SI = dyn_cast<StoreInst>(&I)) {           \
         processPointerOperand (SI->getPointerOperand ());          \
      }                                                             \
    }

    #include "llvm/IR/Instruction.def"

    void visitInstruction(Instruction &I) {
      errs() << "Instruction Count does not know about " << I;
      llvm_unreachable(nullptr);
    }

  public:

    static char ID; 

    AnalysisProfiler() : 
        ModulePass(ID),
        DL (nullptr), TLI (nullptr),
        TotalFuncs (0), TotalBlocks (0), TotalJoins (0), TotalInsts (0),
        TotalDirectCalls (0), TotalExternalCalls (0), TotalIndirectCalls (0),
        ////////
        SafeIntDiv (0), SafeFPDiv (0), 
        UnsafeIntDiv (0), UnsafeFPDiv (0), DivUnknown (0),
        /////////
        TotalMemAccess (0), MemUnknownSize (0), 
        SafeMemAccess (0), UnsafeMemAccess (0), MemUnknown (0),
        ///
        SafeLeftShift (0), UnsafeLeftShift (0), UnknownLeftShift(0)
    { }

    bool runOnFunction(Function &F)  {
       // unsigned StartMemInsts =
       //     NumGetElementPtrInst + NumLoadInst + NumStoreInst + NumCallInst +
       //     NumInvokeInst + NumAllocaInst;
       visit(F);
       // unsigned EndMemInsts =
       //     NumGetElementPtrInst + NumLoadInst + NumStoreInst + NumCallInst +
       //     NumInvokeInst + NumAllocaInst;
       // TotalMemInst += EndMemInsts-StartMemInsts;
       return false;
    }

    bool runOnModule (Module &M) override {

      DL = &getAnalysis<DataLayoutPass>().getDataLayout ();
      TLI = &getAnalysis<TargetLibraryInfo>();

      for (auto &F: M) {
        runOnFunction (F);
      }
      printReport (errs ());

      CanReadUndef undef;
      undef.runOnModule (M);
      undef.printReport (errs ());

      errs () << " ====================================== \n";
      errs () << "   Non-analyzed (external) functions    \n";
      errs () << " ====================================== \n";
      for (auto &p: ExtFuncs) 
           errs () << p.getKey () << "\n";
      return false;
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
      AU.addRequired<llvm::DataLayoutPass>();
      AU.addRequired<llvm::TargetLibraryInfo>();
    }

    void formatCounters (std::vector<Counter>& counters, 
                         unsigned& MaxNameLen, 
                         unsigned& MaxValLen,
                         bool sort = true) {
      // Figure out how long the biggest Value and Name fields are.
      for (auto c: counters) {
        MaxValLen = std::max(MaxValLen,
                             (unsigned)utostr(c.getValue()).size());
        MaxNameLen = std::max(MaxNameLen,
                              (unsigned)std::strlen(c.getName()));
      }

      if (sort) {
        // Sort the fields by name.
        std::stable_sort(counters.begin(), counters.end());
      }
    }

                                                            
    void printReport (raw_ostream &O) {
      unsigned MaxNameLen = 0, MaxValLen = 0;

      { 
        // Global counters
        Counter c1 (1,"Number of instructions");
        c1 += TotalInsts;
        Counter c2 (2,"Number of basic blocks");
        c2 += TotalBlocks;
        Counter c3 (3,"Number of joins");
        c3 += TotalJoins;
        Counter c4 (4,"Number of non-external functions");
        c4 += TotalFuncs;
        Counter c5 (5,"Number of (non-external) direct calls");
        c5 += TotalDirectCalls;
        Counter c6 (6,"Number of (non-external) indirect calls");
        c6 += TotalIndirectCalls;
        Counter c7 (7,"Number of external calls");
        c7 += TotalExternalCalls;

       std::vector<Counter> global_counters {c1, c2, c3, c4, c5, c6, c7};
        formatCounters (global_counters, MaxNameLen, MaxValLen, false);
        for (auto c: global_counters) {
          O << format("%*u %-*s\n",
                      MaxValLen, 
                      c.getValue(),
                      MaxNameLen,
                      c.getName());
        }
      }

      { // instruction counters
        MaxNameLen = MaxValLen = 0;
        std::vector<Counter> inst_counters;
        inst_counters.reserve (counters.size ());
        for(auto &p: counters) { inst_counters.push_back (p.second); }
        formatCounters (inst_counters, MaxNameLen, MaxValLen);
        O << " ===================================== \n";
        O << "  Number of each kind of instructions  \n";
        O << " ===================================== \n";
        for (auto c: inst_counters) {
          O << format("%*u %-*s\n",
                      MaxValLen, 
                      c.getValue(),
                      MaxNameLen,
                      c.getName());
        }
      }

      { // Division counters
        MaxNameLen = MaxValLen = 0;
        Counter c1 (1,"Number of safe integer div/rem");
        c1 += SafeIntDiv;
        Counter c2 (2,"Number of definite unsafe integer div/rem");
        c2 += UnsafeIntDiv;
        Counter c3 (3,"Number of safe FP div/rem");
        c3 += SafeFPDiv;
        Counter c4 (4,"Number of definite unsafe FP div/rem");
        c4 += UnsafeFPDiv;
        Counter c5 (5,"Number of non-static div/rem");
        c5 += DivUnknown;
        std::vector<Counter> div_counters {c1, c2, c3, c4, c5};
        formatCounters (div_counters, MaxNameLen, MaxValLen,false);
        O << " ======================== \n";
        O << "   Division by zero       \n";
        O << " ======================== \n";
        for (auto c: div_counters) {
          O << format("%*u %-*s\n",
                      MaxValLen, 
                      c.getValue(),
                      MaxNameLen,
                      c.getName());
        }
      }

      { // left shift
        MaxNameLen = MaxValLen = 0;
        Counter c1 (1,"Number of safe left shifts");
        c1 += SafeLeftShift;
        Counter c2 (2,"Number of definite unsafe left shifts");
        c2 += UnsafeLeftShift;
        Counter c3 (3,"Number of unknown left shifts");
        c3 += UnknownLeftShift;
        std::vector<Counter> lsh_counters {c1, c2, c3};
        formatCounters (lsh_counters, MaxNameLen, MaxValLen,false);
        O << " ======================== \n";
        O << "   Oversized Left Shifts  \n";
        O << " ======================== \n";
        for (auto c: lsh_counters) {
          O << format("%*u %-*s\n",
                      MaxValLen, 
                      c.getValue(),
                      MaxNameLen,
                      c.getName());
        }
      }


      { // Memory counters
        MaxNameLen = MaxValLen = 0;
        Counter c1 (1,"Total Number of memory accesses (only via Load/Store)");
        c1 += TotalMemAccess;
        Counter c2 (2,"Number of safe memory accesses");
        c2 += SafeMemAccess;
        Counter c3 (3,"Number of definite unsafe memory accesses");
        c3 += UnsafeMemAccess;
        Counter c4 (4,"Number of unknown memory accesses due to unknown size");
        c4 += MemUnknownSize;
        Counter c5 (5,"Number of unknown memory accesses due to unknown offset");
        c5 += MemUnknown;
        std::vector<Counter> mem_counters {c1, c2, c3, c4, c5};
        formatCounters (mem_counters, MaxNameLen, MaxValLen,false);
        O << " ================================= \n";
        O << "   Out-of-bouns memory accesses    \n";
        O << " ================================= \n";
        for (auto c: mem_counters) {
          O << format("%*u %-*s\n",
                      MaxValLen, 
                      c.getValue(),
                      MaxNameLen,
                      c.getName());
        }
      }
    }

    virtual const char* getPassName () const {return "AnalysisProfiler";}
  };

  char AnalysisProfiler::ID = 0;

  ModulePass *createAnalysisProfilerPass() { 
    return new AnalysisProfiler(); 
  }

  } // end namespace crabllvm




