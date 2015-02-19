#include <llvm/IR/Function.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#include <llvm/Transforms/Utils/PromoteMemToReg.h>
#include <llvm/Transforms/Utils/Local.h>

#include <llvm/Pass.h>

#include "tsar_pass.h"

#include <iostream>

using namespace llvm;

/// \brief Print description of a variable from a source code for specified alloca.
void printAllocaSource(raw_ostream &o, AllocaInst *AI) { 
    DbgDeclareInst *DDI = FindAllocaDbgDeclare(AI);
    if (DDI) {
        DIVariable DIVar(DDI->getVariable( ));
        errs( ) << DIVar.getLineNumber( ) << ": ";

        DITypeRef DITy(DIVar.getType( ));
        if (MDNode *MDTy = dyn_cast<MDNode>((Value*) DITy)) {
            DIDescriptor DID(MDTy);
            if (DID.isDerivedType( )) {
                DIDerivedType DIDTy(MDTy);
                errs( ) << DIDTy.getTypeDerivedFrom( ).getName( ) << "* ";
            }
            else
                errs( ) << DITy.getName( ) << " ";
        }
        else
            errs( ) << DITy.getName( ) << " ";

        errs( ) << DIVar.getName( ) << ": ";
    }
    AI->print(errs( ));
    errs( ) << "\n";
}

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

void PrivateRecognitionPass::printLoops(const Twine &offset, LoopInfo::reverse_iterator rbeginItr, LoopInfo::reverse_iterator rendItr) {
    for (; rbeginItr != rendItr; ++rbeginItr) {
        (offset + "- ").print(errs( ));
        DebugLoc loc = (*rbeginItr)->getStartLoc( );
        loc.print(getGlobalContext( ), errs( ));
        errs( ) << "\n";
        printLoops(offset + "\t", (*rbeginItr)->rbegin( ), (*rbeginItr)->rend( ));
    }
}

bool PrivateRecognitionPass::runOnFunction(Function &F) {
    std::vector<AllocaInst*> AnlsAllocas;
    LoopInfo &LpInfo = getAnalysis<LoopInfo>( );
    DominatorTree &DomTree = getAnalysis<DominatorTreeWrapperPass>( ).getDomTree( );

    BasicBlock &BB = F.getEntryBlock( );
    for (BasicBlock::iterator BBItr = BB.begin( ), BBEndItr = --BB.end( ); BBItr != BBEndItr; ++BBItr) {
        auto *AI = dyn_cast<AllocaInst>(BBItr);
        if (AI && isAllocaPromotable(AI))
            AnlsAllocas.push_back(AI);
    }

    if (AnlsAllocas.empty( ))
        return false;

    for (AllocaInst *AI : AnlsAllocas)
        printAllocaSource(errs( ), AI);

    printLoops("", LpInfo.rbegin( ), LpInfo.rend( ));
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
