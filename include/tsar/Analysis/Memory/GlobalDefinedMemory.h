#include <tsar/Analysis/Memory/Passes.h>
#include <tsar/Analysis/Memory/DefinedMemory.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
  class GlobalDefinedMemory : public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;
    GlobalDefinedMemory() : ModulePass(ID) {
      initializeGlobalDefinedMemoryPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module &SCC) override;
    void getAnalysisUsage(AnalysisUsage& AU) const override;

    /// Returns results of reach definition analysis.
    tsar::InterprocDefInfo & getInterprocDefInfo() noexcept { return mInterprocDefInfo; }

    /// Returns results of reach definition analysis.
    const tsar::InterprocDefInfo & getInterprocDefInfo() const noexcept {
      return mInterprocDefInfo;
    }


  private:
    //tsar::DefinedMemoryInfo mDefInfo;
    tsar::InterprocDefInfo mInterprocDefInfo;
  };

}
