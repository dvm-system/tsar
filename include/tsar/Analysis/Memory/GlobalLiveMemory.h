#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
	class GlobalLiveMemory : public ModulePass, private bcl::Uncopyable {
	public:
		static char ID;
		GlobalLiveMemory() : ModulePass(ID) {
			initializeGlobalLiveMemoryPass(*PassRegistry::getPassRegistry());
		}

		bool runOnModule(Module &SCC) override;
		void getAnalysisUsage(AnalysisUsage& AU) const override;
	};
}
