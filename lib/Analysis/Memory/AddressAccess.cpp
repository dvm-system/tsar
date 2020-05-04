#include "tsar/Analysis/Memory/PrivateAnalysis.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DependenceAnalysis.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassProvider.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Debug.h>
#include "tsar/Analysis/Memory/AddressAccess.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/ADT/SCCIterator.h"
#include <iostream>

#undef DEBUG_TYPE
#define DEBUG_TYPE "address-access"

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

char AddressAccessAnalyser::ID = 0;

namespace {
    using FunctionPassesProvider = FunctionPassProvider<DFRegionInfoPass, DefinedMemoryPass, EstimateMemoryPass>;
}

INITIALIZE_PROVIDER_BEGIN(FunctionPassesProvider,
                          "function-passes-provider",
                          "Region,DefineMemory,EstimateMemory -passes (Provider)")
    INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
    INITIALIZE_PASS_DEPENDENCY(DefinedMemoryPass)
    INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PROVIDER_END(FunctionPassesProvider, "function-passes-provider",
                        "Region,DefineMemory,EstimateMemory -passes (Provider)")

INITIALIZE_PASS_IN_GROUP_BEGIN(AddressAccessAnalyser, "address-access",
                               "address-access", false, true,
                               DefaultQueryManager::PrintPassGroup::getPassRegistry())
    INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
    INITIALIZE_PASS_DEPENDENCY(FunctionPassesProvider)
INITIALIZE_PASS_IN_GROUP_END(AddressAccessAnalyser, "address-access",
                             "address-access", false, true,
                             DefaultQueryManager::PrintPassGroup::getPassRegistry())

Pass *llvm::createAddressAccessAnalyserPass() {
    return new AddressAccessAnalyser();
}

void AddressAccessAnalyser::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<GlobalOptionsImmutableWrapper>();
    AU.addRequired<FunctionPassesProvider>();

    AU.setPreservesAll();
}

void AddressAccessAnalyser::runOnFunction(Function &F) {
    FunctionPassesProvider *Provider = &getAnalysis<FunctionPassesProvider>(F);

    // obtain vars address of which is evaluated in function
    tsar::DefinedMemoryInfo *DefInfo = &Provider->get<DefinedMemoryPass>().getDefInfo();
    tsar::DFRegionInfo *RegionInfo = &Provider->get<DFRegionInfoPass>().getRegionInfo();

    auto *FunRegion = cast<DFFunction>(RegionInfo->getTopLevelRegion());
    auto DefItr = DefInfo->find(FunRegion);
    auto &DefUse = DefItr->get<DefUseSet>();

    const PointerSet &ValuesAccessedByAddress = DefUse->getAddressAccesses();

    // keep only those values which intersect with fun-params by memory
    auto ValToArgs = DenseMap<Value *, DenseSet<Argument *>>();

    auto AliasTree = &getAnalysis<EstimateMemoryPass>(F).getAliasTree();
    AliasTreeRelation AliasSTR(AliasTree);

    for (Value *ValueAccessed : ValuesAccessedByAddress) {
        LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: var accessed by address " << ValueAccessed->getName().str()
                          << "\n";);

        const tsar::AliasEstimateNode *ValueAliasNode = getAliasNodeByPointerValue(ValueAccessed, &F, *AliasTree);

        auto args = StoredPtrArguments();
        for (Argument *arg = F.arg_begin(); arg != F.arg_end(); arg++) {
            auto ArgTy = arg->getType();
            if (!ArgTy->isPointerTy()) // we're analyzing only pointers
                continue;
            if (isNonTrivialPointerType(ArgTy))
                args.insert(arg);

            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: par of pointer type " << arg->getName().str()
                              << "\n";);

            const tsar::AliasEstimateNode *ArgumentAliasNode = getAliasNodeByPointerValue(arg, &F, *AliasTree);
            if (!AliasSTR.isUnreachable(ValueAliasNode, ArgumentAliasNode))
                args.insert(arg);
        }

        if (!args.empty())
            ValToArgs.insert(std::pair<Value *, DenseSet<Argument *>>(ValueAccessed, args));
    }

    // iterate over kept usages to see if address is stored somewhere
    // TODO
}

void AddressAccessAnalyser::runOnFunctionBasic(Function &F) {
    auto StoredPtrArgs = StoredPtrArguments();  // if argument is here => ptr held by it may be stored somewhere
    for (Argument *arg = F.arg_begin(); arg != F.arg_end(); arg++) {
        auto ArgTy = arg->getType();
        if (isNonTrivialPointerType(ArgTy))  // TODO: extend analysis for [**int] and similar types
            StoredPtrArgs.insert(arg);
        if (!ArgTy->isPointerTy()) // not a pointer and doesn't contain one within it
            continue;

        for (auto &U : arg->uses()) {

        }
    }
}

bool AddressAccessAnalyser::runOnModule(Module &M) {
    LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: analyze module " << M.getSourceFileName() << "\n";);

    releaseMemory();

    std::cout << M.getSourceFileName() << std::endl; // debug

    CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
    for (scc_iterator<CallGraph *> SCCI = scc_begin(&CG); !SCCI.isAtEnd(); ++SCCI) {
        const std::vector<CallGraphNode *> &nextSCC = *SCCI;
        // TODO: Fun->doesNotRecurse
        assert(nextSCC.size() == 1 && !SCCI.hasLoop() && "Recursion is not supported yet!");

        Function *F = nextSCC.front()->getFunction();

        if (!F) {
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping external node"
                              << "\n";);
            continue;
        }
        if (F->isIntrinsic()) {
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping intrinsic function " << F->getName().str()
                              << "\n";);
            continue;
        }
        if (hasFnAttr(*F, AttrKind::LibFunc)) {
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping lib function " << F->getName().str()
                              << "\n";);
            continue;
        }

        LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: analyzing function " << F->getName().str()
                          << "\n";);
        runOnFunction(*F);
    }

    return false;
}

const tsar::AliasEstimateNode *AddressAccessAnalyser::getAliasNodeByPointerValue(
        Value *Val, Function *F, const tsar::AliasTree &ATree) {

    const DataLayout &DL = F->getParent()->getDataLayout();

    auto PointeeTy = cast<PointerType>(Val->getType())->getElementType();
    auto ValLocation = MemoryLocation(
            Val,
            PointeeTy->isSized() ? DL.getTypeStoreSize(PointeeTy) : MemoryLocation::UnknownSize);

    return ATree.find(ValLocation)->getAliasNode(ATree);
}

bool AddressAccessAnalyser::isNonTrivialPointerType(llvm::Type *Ty) {
  assert(Ty && "Type must not be null!");
  if (Ty->isArrayTy())
    return hasUnderlyingPointer(Ty->getArrayElementType());
  if (Ty->isVectorTy())
    return hasUnderlyingPointer(Ty->getVectorElementType());
  if (Ty->isStructTy())
    for (unsigned I = 0, EI = Ty->getStructNumElements(); I < EI; ++I)
      return hasUnderlyingPointer(Ty->getStructElementType(I));
  return false;
}

//void AddressAccessAnalyser::resolveCandidats(
//        const GraphNumbering<const AliasNode *> &Numbers,
//        const AliasTreeRelation &AliasSTR, DFRegion *R) {
//
//    if (auto *L = dyn_cast<DFLoop>(R)) {
//        auto DefItr = mDefInfo->find(L);
//        assert(DefItr != mDefInfo->end() &&
//               DefItr->get<DefUseSet>() && DefItr->get<ReachSet>() &&
//               "Def-use and reach definition set must be specified!");
//        auto LiveItr = mLiveInfo->find(L);
//        assert(LiveItr != mLiveInfo->end() && LiveItr->get<LiveSet>() &&
//               "List of live locations must be specified!");
//        TraitMap ExplicitAccesses;
//        UnknownMap ExplicitUnknowns;
//        AliasMap NodeTraits;
//        for (auto &N : *mAliasTree)
//            NodeTraits.insert(
//                    std::make_pair(&N, std::make_tuple(TraitList(), UnknownList())));
//        resolveAddresses(L, *DefItr->get<DefUseSet>(), ExplicitAccesses,
//                         ExplicitUnknowns, NodeTraits);
//    }
//
//    for (auto I = R->region_begin(), E = R->region_end(); I != E; ++I)
//        resolveCandidats(Numbers, AliasSTR, *I);
//}

// toask: why we need this information?
//bool AddressAccessAnalyser::runOnSCC(CallGraphSCC &SCC) {
//    releaseMemory();
//
//    // TODO: make conservative + check that fun doesnt call itself
//    assert(SCC.size() == 1 && "Recursion is not supported");  // toask: detects only more-than-one recursion
//
//    for (auto node : SCC) {
//        Function *F = node->getFunction();
//        if (!F)
//            continue;
//
//        if (mParameterAccesses.find(F) == mParameterAccesses.end())
//            mParameterAccesses[F] = llvm::BitVector(F->arg_size());
//
//        std::cout << F->getName().data() << std::endl;
//
//        // toask: don't skip it here
//        auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
////        if (!GlobalOpts.AnalyzeLibFunc && hasFnAttr(*F, AttrKind::LibFunc))
////            return false;
//
//        // TODO: use Provider as in GlobalDefinedMemory
//        // TODO: maybe convert to Module
//        DFRegionInfo &RegionInfo = getAnalysis<DFRegionInfoPass>(*F).getRegionInfo();
//        mDefInfo = &getAnalysis<DefinedMemoryPass>(*F).getDefInfo();
//        mLiveInfo = &getAnalysis<LiveMemoryPass>(*F).getLiveInfo();
//        mAliasTree = &getAnalysis<EstimateMemoryPass>(*F).getAliasTree();
//        auto *DFF = cast<DFFunction>(RegionInfo.getTopLevelRegion());
//        GraphNumbering<const AliasNode *> Numbers;
//        numberGraph(mAliasTree, &Numbers);
//        AliasTreeRelation AliasSTR(mAliasTree);
//        resolveCandidats(Numbers, AliasSTR, DFF);
//    }
//
//    return false;
//}

//BitVector AddressAccessAnalyser::getParametersOriginatedValue(Value *V, Function *F) {
//    auto result = BitVector(F->arg_size(), false);
//    // TODO: match param to value
////    for (int i = 0; i < F->arg_size(); i++) {
////        Value *arg = F->arg_begin()[i];
////        if (V == arg)
////            result.set(i);
////    }
//
//    return result;  // return all for now
//}

//void AddressAccessAnalyser::resolveAddresses(DFLoop *L,
//                                             const DefUseSet &DefUse, TraitMap &ExplicitAccesses,
//                                             UnknownMap &ExplicitUnknowns, AliasMap &NodeTraits) {
//    assert(L && "Loop must not be null!");
//    for (Value *Ptr : DefUse.getAddressAccesses()) {
//        const EstimateMemory *Base = mAliasTree->find(MemoryLocation(Ptr, 0));
//        assert(Base && "Estimate memory location must not be null!");
//        auto Root = Base->getTopLevelParent();
//        // Do not remember an address:
//        // * if it is stored in some location, for example
//        // isa<LoadInst>(Root->front()), locations are analyzed separately;
//        // * if it points to a temporary location and should not be analyzed:
//        // for example, a result of a call can be a pointer.
//        if (!isa<AllocaInst>(Root->front()) && !isa<GlobalValue>(Root->front()))
//            continue;
//        Loop *Lp = L->getLoop();
//        // If this is an address of a location declared in the loop do not
//        // remember it.
//        if (auto AI = dyn_cast<AllocaInst>(Root->front()))
//            if (Lp->contains(AI->getParent()))
//                continue;
//        for (auto &U : Ptr->uses()) {
//            auto *User = U.getUser();
//            if (auto II = dyn_cast<IntrinsicInst>(User))  // skip llvm built-ins, cause they're missing in the result
//                if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
//                    isDbgInfoIntrinsic(II->getIntrinsicID()))
//                    continue;
//            SmallDenseMap<Instruction *, Use *, 1> UseInsts;  // toask: what for?
//            if (auto *CE = dyn_cast<ConstantExpr>(
//                    User)) {  // constexpr are expressions which can be shared by multiple insts
//                SmallVector<ConstantExpr *, 4> WorkList{CE};
//                do {
//                    auto *Expr = WorkList.pop_back_val();
//                    for (auto &ExprU : Expr->uses()) {
//                        auto ExprUse = ExprU.getUser();
//                        if (auto ExprUseInst = dyn_cast<Instruction>(ExprUse))
//                            UseInsts.try_emplace(ExprUseInst, &ExprU);
//                        else if (auto ExprUseExpr = dyn_cast<ConstantExpr>(ExprUse))
//                            WorkList.push_back(ExprUseExpr);
//                    }
//                } while (!WorkList.empty());
//            } else if (auto UI = dyn_cast<Instruction>(User)) {
//                UseInsts.try_emplace(UI, &U);
//            }
//            if (UseInsts.empty())
//                continue;
//            if (!any_of(UseInsts, [Lp, User, this](std::pair<Instruction *, Use *> &I) {
//                if (!Lp->contains(I.first->getParent()))
//                    return false;
//
//                bool usedForAriphmetics = false;
//                // The address is used inside the loop.
//                // Remember it if it is used for computation instead of memory
//                // access or if we do not know how it will be used.
//                if (isa<PtrToIntOperator>(User))  // address converted to int -> 99% it's used in arithemtics then
//                    usedForAriphmetics |= true;
//
//                if (auto *SI = dyn_cast<StoreInst>(I.first))
//                    usedForAriphmetics |= (I.second->getOperandNo() !=
//                                           StoreInst::getPointerOperandIndex());  // address is stored somewhere -> we don't know how it's used further
//                // Address should be also remembered if it is a function parameter, which is used for this
//                ImmutableCallSite CS(I.first);
//                if (CS && CS.getCalledValue() != I.second->get()) {  // toask: what does that mean?
//                    auto *called = dyn_cast<Function>(CS.getCalledValue());
//                    usedForAriphmetics |= mParameterAccesses[called][CS.getArgumentNo(I.second)];
//                }
//
//                if (usedForAriphmetics) {  // TODO: spannerTreeRelation for check if V intersects with Fun[par] by memory
//                    Function *Fun = Lp->getHeader()->getParent();
//                    this->mParameterAccesses[Fun] |= this->getParametersOriginatedValue(I.second->get(), Fun);
//                    // TODO:
//                    // TODO: 1) check if par intersects by memory
//                    // TODO: 2) think about complicated pointer dereference
//                }
//
//                return usedForAriphmetics;
//            }))
//                continue;
//
//            mAccessSet.insert(U.get());
//        }
//    }
//    for (auto *Unknown : DefUse.getAddressUnknowns()) {  // TODO: ??
//        /// Is it safe to ignore intrinsics here? It seems that all intrinsics in
//        /// LLVM does not use addresses to perform  computations instead of
//        /// memory accesses.
//        if (isa<IntrinsicInst>(Unknown))
//            continue;
//        const auto *N = mAliasTree->findUnknown(Unknown);
//        assert(N && "Alias node for unknown memory location must not be null!");
//        auto Pair = ExplicitUnknowns.try_emplace(Unknown);
//        if (!Pair.second) {
//            *Pair.first->get<BitMemoryTrait>() &= BitMemoryTrait::AddressAccess;
//        } else {
//            auto I = NodeTraits.find(N);
//            I->get<UnknownList>().push_front(std::make_pair(
//                    Unknown, BitMemoryTrait::NoRedundant & BitMemoryTrait::NoAccess &
//                             BitMemoryTrait::AddressAccess));
//            Pair.first->get<BitMemoryTrait>() =
//                    &I->get<UnknownList>().front().get<BitMemoryTrait>();
//            Pair.first->get<AliasNode>() = N;
//        }
//    }
//}