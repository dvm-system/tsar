
#ifndef SAPFOR_RESTRICTIONARGUMENTSPASS_H
#define SAPFOR_RESTRICTIONARGUMENTSPASS_H

#include <llvm/Pass.h>
#include <bcl/utility.h>
#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>

namespace llvm {

    class RestrictionArgumentsPass : public ModulePass, private bcl::Uncopyable {
    public:
        static char ID;

        RestrictionArgumentsPass() : ModulePass(ID) {
            initializeRestrictionArgumentsPassPass(*PassRegistry::getPassRegistry());
        }

        bool runOnModule(llvm::Module& M) override;

        void getAnalysisUsage(AnalysisUsage& AU) const override;
    };

}

namespace tsar {

//  llvm::DenseSet<llvm::Value*> showed unstable behaviour in sets intersection
    typedef std::set<llvm::Value*> MemorySources;

    struct FunctionCallWithMemorySources {
        llvm::CallInst* mCallI;
        llvm::DenseMap<int, MemorySources> mArgumentMemorySources;

        FunctionCallWithMemorySources() :
            mCallI(nullptr),
            mArgumentMemorySources(llvm::DenseMap<int, MemorySources>()) {}

        explicit FunctionCallWithMemorySources(llvm::CallInst* CallI) :
            mCallI(CallI),
            mArgumentMemorySources(llvm::DenseMap<int, MemorySources>()) {}
    };

    struct ValueWithMemorySources {
        llvm::Value* mV;
        MemorySources mMemorySources;

        explicit ValueWithMemorySources(llvm::Value* V) :
            mV(V) {}

        ValueWithMemorySources(llvm::Value* V, llvm::Value* MemorySource) :
            mV(V) {
            mMemorySources.insert(MemorySource);
        }

        ValueWithMemorySources(llvm::Value* V, MemorySources Sources) :
            mV(V) {
            mMemorySources.insert(Sources.begin(), Sources.end());
        }
    };

    struct FunctionResultArgumentsMemoryDependencies {
        llvm::DenseSet<int> mInfluencingArgumentsIndexes;
        // Indicates is returned value always points to the unique memory
        bool mIsRestrict;

        FunctionResultArgumentsMemoryDependencies() :
            mIsRestrict(false) {}
    };

    struct FunctionArgumentRestrictCalls {
        int mArgumentIndex;
        llvm::DenseSet<llvm::CallInst*> mRestrictCalls;
        bool mAllFunctionCallsRestrict;

        FunctionArgumentRestrictCalls(int index) :
            mArgumentIndex(index),
            mAllFunctionCallsRestrict(false) {}
    };

    struct FunctionCallsInfo {
        llvm::Function* mFunction;
        llvm::DenseMap<int, FunctionArgumentRestrictCalls> mRestrictCallsByArguments;
    };

    //typedef llvm::DenseSet<int> FunctionResultArgumentsMemoryDependencies;

}



#endif //SAPFOR_RESTRICTIONARGUMENTSPASS_H
