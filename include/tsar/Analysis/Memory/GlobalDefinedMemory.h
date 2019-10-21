#include "tsar_pass.h"
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
  class GlobalDefinedMemory : public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;
    GlobalDefinedMemory() : ModulePass(ID) {
      initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module &SCC) override;
    void getAnalysisUsage(AnalysisUsage& AU) const override;

  private:
    tsar::DefinedMemoryInfo mDefInfo;
    //tsar::InterprocDefInfo mInterprocDefInfo;
  };

}
