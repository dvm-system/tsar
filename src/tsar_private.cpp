#include <llvm/IR/Function.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <llvm/Pass.h>

#include "tsar_pass.h"

#include <iostream>

using namespace llvm;

namespace
{
    struct PrivateRecognitionPass : public FunctionPass 
    {
        static char ID;
        PrivateRecognitionPass( ) : FunctionPass(ID) {
            initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry( ));
        }

        bool runOnFunction(Function &F) override;

        void getAnalysisUsage(AnalysisUsage &AU) const override 
        {
            AU.addRequired<DominatorTreeWrapperPass>( );
            AU.addRequired<LoopInfo>( );
            AU.setPreservesAll( );
        }
    private:
        void printLoops(const Twine &offset, LoopInfo::reverse_iterator rbeginItr, LoopInfo::reverse_iterator rendItr);
    };
}

char PrivateRecognitionPass::ID = 0;

INITIALIZE_PASS_BEGIN(PrivateRecognitionPass, "private", "Private Variable Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_END(PrivateRecognitionPass, "private", "Private Variable Analysis", true, true)

FunctionPass *llvm::createPrivateRecognitionPass( ) 
{
    return new PrivateRecognitionPass( );
}

void PrivateRecognitionPass::printLoops(const Twine &offset, LoopInfo::reverse_iterator rbeginItr, LoopInfo::reverse_iterator rendItr)
{
    for (; rbeginItr != rendItr; ++rbeginItr)
    {
        (offset + "- ").print(errs( ));
        DebugLoc loc = (*rbeginItr)->getStartLoc( );
        loc.print(getGlobalContext( ), errs( ));
        errs( ) << "\n";
        printLoops(offset + "\t", (*rbeginItr)->rbegin( ), (*rbeginItr)->rend( ));
    }
}

bool PrivateRecognitionPass::runOnFunction(Function &rtn)
{
    std::vector<AllocaInst*> anlsAllocaColl;

    LoopInfo &loopInfo = getAnalysis<LoopInfo>();
    DominatorTree &domTree = getAnalysis<DominatorTreeWrapperPass>( ).getDomTree( );

    BasicBlock &basicBlock = rtn.getEntryBlock( );
    for (BasicBlock::iterator bbItr = basicBlock.begin( ), bbEndItr = --basicBlock.end( ); bbItr != bbEndItr; ++bbItr)
    {
        AllocaInst *allocaStmt = dyn_cast<AllocaInst>(bbItr);
        if (allocaStmt && isAllocaPromotable(allocaStmt))
            anlsAllocaColl.push_back(allocaStmt);
    }

    if (anlsAllocaColl.empty( ))
        return false;

    for (AllocaInst *allocaInst : anlsAllocaColl)
        allocaInst->dump( );

    printLoops("", loopInfo.rbegin( ), loopInfo.rend( ));
#if 0
    for (LoopInfo::iterator I = LI.begin( ), E = LI.end( ); I != E; ++I)
    {
        errs( ) << "LOOP ";
        DebugLoc loc = (*I)->getStartLoc( );
        loc.print(getGlobalContext( ), errs( ));

        for (LoopInfo::iterator childItr = (*I)->begin( );
             childItr != (*I)->end( );
             ++childItr)
        {
            errs( ) << "\tLOOP ";
            DebugLoc loc = (*childItr)->getStartLoc( );
            loc.print(getGlobalContext( ), errs( ));
            errs( ) << "\n";
        }

#if 0
        BasicBlock *loopHeaderBB = (*I)->getHeader( );
        for (BasicBlock::iterator stmtItr = loopHeaderBB->begin( );
             stmtItr != loopHeaderBB->end( );
             ++stmtItr)
        {
            if (MDNode *mdNode = stmtItr->getMetadata("dbg"))
            {
                DILocation stmtLoc(mdNode);
                unsigned line = stmtLoc.getLineNumber( );
                StringRef file = stmtLoc.getFilename( );
                StringRef dir = stmtLoc.getDirectory( );

                std::cerr << "\tline " << line << " file " << file.data() << " directory " <<  dir.data( ) << std::endl;
            }
        }
#endif
    }
#endif
    return false; 
}
