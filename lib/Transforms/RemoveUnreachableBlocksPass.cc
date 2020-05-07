#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Local.h"

#include "clam/config.h"

#include "seadsa/DsaAnalysis.hh"
#include "seadsa/ShadowMem.hh"

using namespace llvm;

namespace clam {
struct RemoveUnreachableBlocksPass : public FunctionPass {
  static char ID;
  RemoveUnreachableBlocksPass() : FunctionPass(ID) {}

  bool runOnFunction(Function &F) { return removeUnreachableBlocks(F); }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    // Preserve Sea-DSA passes
    AU.addPreservedID(seadsa::DsaAnalysis::ID);
    AU.addPreservedID(seadsa::ShadowMemPass::ID);
  }

  virtual StringRef getPassName() const {
    return "Clam: Remove unreachable blocks";
  }
};

char RemoveUnreachableBlocksPass::ID = 0;
Pass *createRemoveUnreachableBlocksPass() {
  return new RemoveUnreachableBlocksPass();
}
} // namespace clam
