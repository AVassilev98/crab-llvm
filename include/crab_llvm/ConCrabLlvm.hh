#ifndef __CONC_CRAB_LLVM_HPP_
#define __CONC_CRAB_LLVM_HPP_

/* 
 * Infer invariants using Crab for concurrent programs.
 * Really experimental ...
 */

#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include "crab_llvm/MemAnalysis.hh"

#include "crab/cfg/conc_sys.hpp"

namespace crab_llvm
{

  using namespace llvm;
  using namespace crab::cfg_impl;

  class ConCrabLlvm : public llvm::ModulePass
  {
    // --- used by the builder of each cfg
    boost::shared_ptr<MemAnalysis> m_mem;    

    // --- threads in the programs
    set<Function*> m_threads;
 
    // --- map a shared variable to the threads that read/write on it
    DenseMap<const Value*, std::set<const Function*> > m_shared_vars;

   public:

    static char ID;        
    
    ConCrabLlvm (): 
        llvm::ModulePass (ID), 
        m_mem (new DummyMemAnalysis ()) { }

    virtual bool runOnModule (llvm::Module& M);

    virtual const char* getPassName () const {return "ConCrabLlvm";}

    virtual void getAnalysisUsage (llvm::AnalysisUsage &AU) const ;

   private:

    bool processFunction (llvm::Function &F);

  };

} // end namespace

#endif
