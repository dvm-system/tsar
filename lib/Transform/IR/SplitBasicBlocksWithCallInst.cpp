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
  //errs() << "==========\nSplitter\n";
  //errs() << F.getName();
  //errs() << "\n==========\n";
  if (F.hasName() && !F.empty()) {
    for (auto currBB = F.begin(), lastBB = F.end(); currBB != lastBB; ++currBB) {
      //errs() << "BBname: " << currBB->getName() << "\n";
      for (auto currInstr = currBB->begin(), lastInstr = currBB->end();
        currInstr != lastInstr; ++currInstr) {
        Instruction *i = &*currInstr;
        Instruction *li = &*lastInstr;
        BasicBlock* newBB;
        if (auto* callInst = dyn_cast<CallInst>(i)) {
          if (i != li  && currBB->getTerminator()) {
            //errs() << "Befor split\n";
            //F.dump();
            if (i == &*(currBB->begin())) {
              newBB = &*currBB;
            } else {
              newBB = currBB->splitBasicBlock(callInst);
              //errs() << "After first split\n";
              //F.dump();
            }
          }
          auto nextInstr = callInst->getNextNonDebugInstruction();
          if (nextInstr != &*(newBB->end()) && newBB->getTerminator()) {
            newBB->splitBasicBlock(nextInstr);
            //errs() << "After second split\n";
            //F.dump();
            currBB++;
            break;
          }         
        }
      }
    }
    //F.dump();
  }

  return true;
}