#include <tsar/Transform/IR/Passes.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
class SplitBasicBlocksWithCallInstPass: public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  SplitBasicBlocksWithCallInstPass() :FunctionPass(ID) {
    initializeSplitBasicBlocksWithCallInstPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

};
}