#include "tsar_pass.h" //файлы анализатора с кавычками
#include <llvm/Pass.h>
#include <bcl/utility.h>
namespace llvm
{
	class testpass: public ModulePass, private bcl::Uncopyable{
		public:
		static char ID; //required
		testpass():ModulePass(ID){
			initializetestpassPass(*PassRegistry::getPassRegistry());
		}
		//нужны точка входа в проход
		bool runOnModule(Module & F) override;
		//задает зависимости прохода
		void getAnalysisUsage(AnalysisUsage & AU) const override;

	};

}
