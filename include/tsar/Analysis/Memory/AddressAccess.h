#ifndef SAPFOR_ADDRESSACCESS_H
#define SAPFOR_ADDRESSACCESS_H

#include "tsar/Analysis/DataFlowGraph.h"
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/ADT/GraphNumbering.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DFMemoryLocation.h"
#include "tsar/Analysis/Memory/IRMemoryTrait.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/Passes.h"
#include <bcl/utility.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Pass.h>
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include <forward_list>
#include <tuple>

namespace tsar {
    class AliasNode;

    class AliasTree;

    class DefUseSet;

    class DFLoop;

    class EstimateMemory;

    class BitMemoryTrait;

    template<class GraphType>
    class SpanningTreeRelation;

/// This determine relation between two nodes in an alias tree.
    using AliasTreeRelation = SpanningTreeRelation<const AliasTree *>;

/// Information about privatizability of locations for an analyzed region.
    using PrivateInfo =
    llvm::DenseMap<DFNode *, DependenceSet,
            llvm::DenseMapInfo<DFNode *>,
            TaggedDenseMapPair <
            bcl::tagged<DFNode *, DFNode>,
            bcl::tagged<DependenceSet, DependenceSet>>>;

    namespace detail {
        class DependenceImp;

        struct DependenceCache;
    }
}

namespace llvm {

    inline namespace tsar_impl {
        class Dependence;

        class DependenceInfo;
    }

    class DataLayout;
    class ScalarEvolution;

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
        using FunctionToArguments = DenseMap<llvm::Function *, StoredPtrArguments *>;
        using DependentInfo = DenseMap<Instruction *, InstrDependencies * >;
        typedef llvm::SmallPtrSet<llvm::Value *, 32> PointerSet;

        static bool isNonTrivialPointerType(Type *);
    public:
        static char ID;

        AddressAccessAnalyser() : ModulePass(ID) {
            initializeAddressAccessAnalyserPass(*PassRegistry::getPassRegistry());
        }

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
        FunctionToArguments mParameterAccesses;
    private:
        DependentInfo *mDepInfo = nullptr;
    };
}
#endif //SAPFOR_ADDRESSACCESS_H
