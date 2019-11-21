#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <tsar/Transform/IR/SplitBasicBlocksWithCallInst.h>

using namespace llvm;

char SplitBasicBlocksWithCallInstPass::ID = 0;
INITIALIZE_PASS_BEGIN(SplitBasicBlocksWithCallInstPass, "Split-BB",
  "Split BB in call instraction", false, true)
INITIALIZE_PASS_END(SplitBasicBlocksWithCallInstPass, "Split-BB",
    "Split BB in call instraction", false, true)

void SplitBasicBlocksWithCallInstPass::getAnalysisUsage(AnalysisUsage & AU) const {
}

FunctionPass * llvm::createSplitBasicBlocksWithCallInstPass() {
  return new SplitBasicBlocksWithCallInstPass();
}

bool SplitBasicBlocksWithCallInstPass::runOnFunction(Function& F) {
	errs() << "==========\nSplitter\n";
	errs() << F.getName();
	errs() << "\n==========\n";
	F.dump();
	errs() << "\n==========\n";
	if (F.hasName() && !F.empty()) {
		for (auto currBB = F.begin(), lastBB = F.end(); currBB != lastBB; ++currBB) {
			errs() << "Current BBname: " << currBB->getName() << "\n";
			TerminatorInst* ti = currBB->getTerminator();

			for (auto currInstr = currBB->begin(), lastInstr = currBB->end();
				currInstr != lastInstr; ++currInstr) {
				if (ti != nullptr ) {
					errs() << "Current Instr:\n";
					currInstr->dump();
					Instruction* i = &*currInstr;
					if (i == ti)
						break;
					BasicBlock* newBB;
					errs() << "Is callInst?\n";
					if (auto* callInst = dyn_cast<CallInst>(i)) {
						errs() << "Is not last inst?\n";
						if (i != ti) {
							errs() << "Befor split\n";
							if (i == &*(currBB->begin())) {
								errs() << "Is First";
								newBB = &*currBB;
							}
							else {
								newBB = currBB->splitBasicBlock(callInst);
							}
						}
						errs() << "get next";
						auto nextInstr = callInst->getNextNonDebugInstruction();
						errs() << "Is not last inst?\n";
						if (nextInstr != ti) {
							newBB->splitBasicBlock(nextInstr);
							currBB++;
							break;
						}
					}
				}
			}
		}
		errs() << "\n==========\n";
		F.dump();
	}

	return true;
}