#ifndef SAPFOR_ADDRESSACCESS_H
#define SAPFOR_ADDRESSACCESS_H

#include "tsar/Analysis/DataFlowGraph.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Pass.h>
#include <forward_list>
#include <tuple>

namespace tsar {
    struct PreservedParametersInfo {
        using StoredPtrArguments = llvm::DenseSet<int>;
        using FunctionToArguments = llvm::DenseMap<const llvm::Function *, StoredPtrArguments *>;

        FunctionToArguments infoByFun;
        PreservedParametersInfo() : infoByFun(FunctionToArguments()) {};

        void addFunction(llvm::Function* F) {infoByFun[F] = new StoredPtrArguments();}
        bool isPreserved(llvm::ImmutableCallSite CS, llvm::Use* use);
    };
}

namespace llvm {

    class MoveUndefined : public std::exception {};

    struct InstrDependencies {
        std::vector<Instruction *> deps;
        bool is_invalid;

        InstrDependencies() : deps(std::vector<Instruction *>()), is_invalid(false) {};
        explicit InstrDependencies(bool is_invalid) : deps(std::vector<Instruction *>()), is_invalid(is_invalid) {};
    };
    class AddressAccessAnalyser :
            public ModulePass, private bcl::Uncopyable {

        using ValueSet = DenseSet<llvm::Value *>;
        using ArgumentHolders = DenseMap<Argument *, ValueSet>;
        using StoredPtrArguments = DenseSet<int>;
        using FunctionToArguments = DenseMap<const llvm::Function *, StoredPtrArguments *>;
        using DependentInfo = DenseMap<Instruction *, InstrDependencies * >;
        typedef llvm::SmallPtrSet<llvm::Value *, 32> PointerSet;

        static bool isNonTrivialPointerType(Type *);
    public:
        static char ID;

        AddressAccessAnalyser() : ModulePass(ID) {
            initializeAddressAccessAnalyserPass(*PassRegistry::getPassRegistry());
        }

        tsar::PreservedParametersInfo &getAA() { return mParameterAccesses; };

        bool isStored(Value *);

        void initDepInfo(Function *);

        std::vector<Value *> useMovesTo(Value *, Instruction *);

        bool contructUsageTree(Argument *arg);

        void runOnFunction(Function *F);

        bool runOnModule(Module &M) override;

        void getAnalysisUsage(AnalysisUsage &AU) const override;

        void print(raw_ostream &OS, const Module *M) const override;

        void releaseMemory() override {};

        // если номер аргумента присутствует, значит он может быть сохранен
        tsar::PreservedParametersInfo mParameterAccesses;
    private:
        DependentInfo *mDepInfo = nullptr;
    };

using AddressAccessAnalyserWrapper =
    AnalysisWrapperPass<tsar::PreservedParametersInfo>;
}

#endif //SAPFOR_ADDRESSACCESS_H
