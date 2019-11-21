#include <tsar/Analysis/Memory/Passes.h>
#include <tsar/Analysis/Memory/LiveMemory.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {

  class GlobalLiveMemory : public ModulePass, private bcl::Uncopyable {
	public:
		static char ID;

    typedef DenseMap<const llvm::Function*, std::unique_ptr<tsar::LiveSet>> IterprocLiveMemoryInfo;

		GlobalLiveMemory() : ModulePass(ID){
			initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
      mIterprocLiveMemoryInfo = nullptr;
		}

		bool runOnModule(Module &SCC) override;
		void getAnalysisUsage(AnalysisUsage& AU) const override;

    const IterprocLiveMemoryInfo* getIterprocLiveMemoryInfo() const noexcept {
      return mIterprocLiveMemoryInfo;
    }

    IterprocLiveMemoryInfo* getIterprocLiveMemoryInfo() noexcept {
      return mIterprocLiveMemoryInfo;
    }

  private:
    IterprocLiveMemoryInfo *mIterprocLiveMemoryInfo;
	};

  typedef GlobalLiveMemory::IterprocLiveMemoryInfo IterprocLiveMemoryInfo;

}
