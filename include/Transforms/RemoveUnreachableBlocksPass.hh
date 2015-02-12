#ifndef REMOVEUNREACHABLEBLOCKSPASS_H
#define REMOVEUNREACHABLEBLOCKSPASS_H

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"

namespace llvm_ikos
{
  using namespace llvm;
  
  struct RemoveUnreachableBlocksPass : public FunctionPass
  {
    static char ID;
    RemoveUnreachableBlocksPass () : FunctionPass (ID) {}
    
    bool runOnFunction (Function &F);
    void getAnalysisUsage (AnalysisUsage &AU) const;
  };
}

#endif /* REMOVEUNREACHABLEBLOCKSPASS_H */
