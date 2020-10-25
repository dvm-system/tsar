#include <llvm/IR/Dominators.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/InstIterator.h>
#include <algorithm>
#include <iterator>
#include <utility>
#include <llvm/Analysis/CallGraph.h>
#include "tsar/Analysis/Memory/RestrictionArgumentsPass.h"
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/Operator.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "restriction-args"

char RestrictionArgumentsPass::ID = 0;
INITIALIZE_PASS_BEGIN(RestrictionArgumentsPass, "restriction-arguments",
    "", false, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_END(RestrictionArgumentsPass, "restriction-arguments",
    "", false, true)

DenseSet<Value*> collectGlobalMemorySources(Module* M) {
  DenseSet<Value *> GlobalMemorySources;
  for (auto &GV : M->getGlobalList()) {
    if (GV.getType()->isPointerTy()) {
      GlobalMemorySources.insert(&GV);
    }
  }
  return GlobalMemorySources;
}

DenseSet<Value*> collectMemorySources(Function* F) {
    DenseSet<Value*> MemorySources;
    for (auto& I : instructions(F)) {

        //collect arrays static allocations
        if (auto* AllocaI = dyn_cast<AllocaInst>(&I)) {
            if (auto* PointerAllocaTy = dyn_cast<PointerType>(AllocaI->getType())) {
                MemorySources.insert(&I);
            }
        }

        //collect malloc-like allocations
        if (auto* CallI = dyn_cast<CallInst>(&I)) {
            if (auto* PointerAllocaTy = dyn_cast<PointerType>(CallI->getType())) {
                MemorySources.insert(&I);
            }
        }
    }
    return MemorySources;
}

int countPointerArguments(Function* F) {
    int PointerArgsAmount = 0;
    for (auto& Arg : F->args()) {
        if (Arg.getType()->isPointerTy()) {
            PointerArgsAmount++;
        }
    }
    return PointerArgsAmount;
}

DenseSet<Value*> findPointerArguments(Function* F) {
    DenseSet<Value*> PointerArgs;
    for (auto& Arg : F->args()) {
        if (Arg.getType()->isPointerTy()) {
            PointerArgs.insert(&Arg);
        }
    }
    return PointerArgs;
}

DenseSet<int> findPointerArgumentsIndexes(Function* F) {
    DenseSet<int> PointerArgsIndexes;
    int i = 0;
    for (auto& Arg : F->args()) {
        if (Arg.getType()->isPointerTy()) {
            PointerArgsIndexes.insert(i);
        }
        i++;
    }
    return PointerArgsIndexes;
}

DenseSet<CallInst*> collectCallsOfFunctionsWithPointerArguments(Function* F) {
    DenseSet<CallInst*> CallsOfFunctionsWithPointerArguments;
    for (auto& I : instructions(F)) {
        if (auto* CallI = dyn_cast<CallInst>(&I)) {
            auto* CalledF = CallI->getCalledFunction();
//            LLVM_DEBUG(
//                dbgs() << "\tCallInst";
//            CallI->dump();
//            dbgs() << "\tCalling Function: ";
//            CalledF->dump();
//            );
            if (!CalledF->isIntrinsic()) {
                int PointerArgsAmount = countPointerArguments(CalledF);
                if (PointerArgsAmount > 0) {
                    CallsOfFunctionsWithPointerArguments.insert(CallI);
                }
            }
        }
    }
    return CallsOfFunctionsWithPointerArguments;
}

DenseMap<CallInst*, FunctionCallWithMemorySources> initCallsToPtrArgumentsMemorySources(DenseSet<CallInst*>& FunctionsCalls) {
    DenseMap<CallInst*, FunctionCallWithMemorySources> CallsToPtrArgumentsMemorySources;
    for (auto* CallI : FunctionsCalls) {
        CallsToPtrArgumentsMemorySources.insert(std::make_pair(CallI, FunctionCallWithMemorySources(CallI)));
    }
    return CallsToPtrArgumentsMemorySources;
}

DenseMap<CallInst*, FunctionCallWithMemorySources> fillCallsToPtrArgumentsMemorySources(
    DenseSet<CallInst*>& TargetFunctionsCalls,
    DenseSet<Value*>& MemorySources,
    DenseMap<Function*, FunctionResultArgumentsMemoryDependencies>& FunctionsReturnValuesDependencies) {
    auto CallsToPtrArgumentsMemorySources = initCallsToPtrArgumentsMemorySources(TargetFunctionsCalls);
    //First inst in pair is a workpiece, second - its memory source
    SmallVector<ValueWithMemorySources, 16> workList;
    for (auto* I : MemorySources) {
        workList.push_back(ValueWithMemorySources(I, I));
    }
    while (!workList.empty()) {
        auto WorkPair = workList.back();
        auto* I = WorkPair.mV;
        auto& memorySources = WorkPair.mMemorySources;
        workList.pop_back();
        for (auto* U : I->users()) {
            if (auto* GepU = dyn_cast<GEPOperator>(U)) {
                workList.push_back(ValueWithMemorySources(GepU, memorySources));
            } else if (auto* CastI = dyn_cast<CastInst>(U)) {
                workList.push_back(ValueWithMemorySources(CastI, memorySources));
            } else if (auto* SelectI = dyn_cast<SelectInst>(U)) {
                workList.push_back(ValueWithMemorySources(SelectI, memorySources));
            } else if (auto* LoadI = dyn_cast<LoadInst>(U)) {
              workList.push_back(ValueWithMemorySources(LoadI, memorySources));
            } else if (auto* CallI = dyn_cast<CallInst>(U)) {
                if (TargetFunctionsCalls.count(CallI)) {
                    LLVM_DEBUG(
                        dbgs() << "\tFound CallInst in TargetCalls: ";
                    CallI->dump();
                    dbgs() << "\tMemory Sources: \n";
                    for (auto* S : memorySources) {
                        dbgs() << "\t\t";
                        S->dump();
                    }
                    );
                    for (int i = 0; i < CallI->getNumArgOperands(); i++) {
                        auto* Arg = CallI->getArgOperand(i);
                        if (Arg == I) {
                            if (CallsToPtrArgumentsMemorySources[CallI].mArgumentMemorySources.count(i)) {
                                CallsToPtrArgumentsMemorySources[CallI].mArgumentMemorySources[i].insert(memorySources.begin(), memorySources.end());
                            }
                            else {
                                CallsToPtrArgumentsMemorySources[CallI].mArgumentMemorySources.insert(
                                    std::make_pair(i, memorySources)
                                );
                            }
                        }
                    }
                }
                auto* TargetFunction = CallI->getCalledFunction();
                if (FunctionsReturnValuesDependencies.count(TargetFunction) &&
                    !FunctionsReturnValuesDependencies[TargetFunction].mInfluencingArgumentsIndexes.empty()) {
                    auto& InfluencingArgumentsIndexes = FunctionsReturnValuesDependencies[TargetFunction].mInfluencingArgumentsIndexes;
                    bool shouldProcess = false;
                    for (int i = 0; i < CallI->getNumArgOperands(); i++) {
                        auto* Arg = CallI->getArgOperand(i);
                        if (Arg == I && InfluencingArgumentsIndexes.count(i)) {
                            shouldProcess = true;
                        }
                    }
                    if (shouldProcess) {
                        workList.push_back(ValueWithMemorySources(CallI, memorySources));
                    }
                }
            }
        }
    }
    return CallsToPtrArgumentsMemorySources;
}

// Maps Function to calls info for each pointer argument index
DenseMap<Function*, DenseMap<int, DenseSet<CallInst*>>> collectRestrictFunctionsCallsByArguments(
    DenseMap<CallInst*, FunctionCallWithMemorySources>& CallsToPtrArgumentsMemorySources,
    DenseMap<Function*, FunctionResultArgumentsMemoryDependencies>& FunctionsReturnValuesDependencies) {
    DenseMap<Function*, DenseMap<int, DenseSet<CallInst*>>> RestrictFunctionsCalls;

    for (auto& CallInfo : CallsToPtrArgumentsMemorySources) {
        auto* CallI = CallInfo.second.mCallI;
        auto& ArgumentsSources = CallInfo.second.mArgumentMemorySources;
        auto* F = CallI->getCalledFunction();

        std::vector<int> ArgumentsIndexes;
        DenseMap<int, DenseSet<int>> ArgumentsCollisions;
        for (auto& S : ArgumentsSources) {
            ArgumentsIndexes.push_back(S.first);
            ArgumentsCollisions.insert(std::make_pair(S.first, DenseSet<int>()));
        }

        for (int i = 0; i < ArgumentsIndexes.size(); i++) {
            for (int j = i + 1; j < ArgumentsIndexes.size(); j++) {
                auto argumentIndex = ArgumentsIndexes[i];
                auto otherArgumentIndex = ArgumentsIndexes[j];
                auto& argumentSources = ArgumentsSources[argumentIndex];
                auto& otherArgumentSources = ArgumentsSources[otherArgumentIndex];
                LLVM_DEBUG(
                    dbgs() << "\tSources for intersection: \n\t\tFirst arg sources:\n";
                    for (auto* S : argumentSources) {
                      dbgs() << "\t\t\t";
                      S->dump();
                    }
                    dbgs() << "\t\tSecond arg sources:\n";
                    for (auto* S : otherArgumentSources) {
                      dbgs() << "\t\t\t";
                      S->dump();
                    }
                );
                std::vector<Value*> sourcesIntersection;
                std::set_intersection(
                    argumentSources.begin(),
                    argumentSources.end(),
                    otherArgumentSources.begin(),
                    otherArgumentSources.end(),
                    std::back_inserter(sourcesIntersection));
                LLVM_DEBUG(
                    if (sourcesIntersection.empty()) {
                      dbgs() << "\t\tEmpty\n";
                    } else {
                      for (auto *S : sourcesIntersection) {
                        dbgs() << "\t\t";
                        S->dump();
                      }
                    }
                );

                if (!sourcesIntersection.empty()) {
                    bool allArgumentSourcesReturnUniqueValues = true;
                    for (auto* commonSource : sourcesIntersection) {
                        if (auto* AllocaI = dyn_cast<AllocaInst>(commonSource)) {
                            allArgumentSourcesReturnUniqueValues = false;
                            break;
                        }
                        else if (auto* CommonCallI = dyn_cast<CallInst>(commonSource)) {
                            auto* TargetCommonFunction = CommonCallI->getCalledFunction();
                            if (FunctionsReturnValuesDependencies.count(TargetCommonFunction)) {
                                if (!FunctionsReturnValuesDependencies[TargetCommonFunction].mIsRestrict) {
                                    allArgumentSourcesReturnUniqueValues = false;
                                    break;
                                }
                            }
                            else if (!TargetCommonFunction->returnDoesNotAlias()) {
                                allArgumentSourcesReturnUniqueValues = false;
                                break;
                            }
                        }
                        else {
                            allArgumentSourcesReturnUniqueValues = false;
                            break;
                        }
                    }
                    if (!allArgumentSourcesReturnUniqueValues) {
                        ArgumentsCollisions[argumentIndex].insert(otherArgumentIndex);
                        ArgumentsCollisions[otherArgumentIndex].insert(argumentIndex);
                    }
                }
            }
        }

        DenseSet<int> RestrictArgumentsForCall;
        for (auto& A : ArgumentsCollisions) {
            if (A.second.empty()) {
                RestrictArgumentsForCall.insert(A.first);
            }
        }

        if (!RestrictFunctionsCalls.count(F)) {
            RestrictFunctionsCalls.insert(std::make_pair(F, DenseMap<int, DenseSet<CallInst*>>()));
        }

        for (auto ArgumentIndex : RestrictArgumentsForCall) {
            if (!RestrictFunctionsCalls[F].count(ArgumentIndex)) {
                RestrictFunctionsCalls[F].insert(std::make_pair(ArgumentIndex, DenseSet<CallInst*>({ CallI })));
            }
            else {
                RestrictFunctionsCalls[F][ArgumentIndex].insert(CallI);
            }
        }
    }
    return RestrictFunctionsCalls;
}

// Maps function to restrict arguments, based on target functions calls
DenseMap<Function*, DenseSet<int>> collectRestrictFunctionsInfoByArguments(
    DenseMap<Function*, DenseMap<int, DenseSet<CallInst*>>>& RestrictFunctionsCalls, DenseSet<CallInst*>& TargetFunctionsCalls) {
    DenseMap<Function*, DenseSet<int>> RestrictFunctionsInfo;
    DenseSet<Function*> PartiallyRestrictFunctions;
    for (auto& FunctionCallsInfo : RestrictFunctionsCalls) {
        auto* F = FunctionCallsInfo.first;
        auto ArgumentsCallsMap = FunctionCallsInfo.second;
        int FCallsCount = 0;
        for (auto* TFC : TargetFunctionsCalls) {
            if (TFC->getCalledFunction() == F) {
                FCallsCount++;
            }
        }
        for (auto& ArgumentCallsInfo : ArgumentsCallsMap) {
            auto ArgumentIndex = ArgumentCallsInfo.first;
            auto& RestrictCalls = ArgumentCallsInfo.second;
            if (FCallsCount == RestrictCalls.size()) {
                if (RestrictFunctionsInfo.count(F)) {
                    RestrictFunctionsInfo[F].insert(ArgumentIndex);
                }
                else {
                    RestrictFunctionsInfo.insert(std::make_pair(F, DenseSet<int>({ ArgumentIndex })));
                }
            }
        }
    }
    return RestrictFunctionsInfo;
}

void runOnFunction(
    Function* F,
    DenseMap<Function*, DenseSet<int>>& KnownRestrictFunctions,
    DenseMap<Function*, DenseSet<int>>& KnownNonRestrictFunctions,
    DenseMap<Function*, FunctionResultArgumentsMemoryDependencies>& FunctionsReturnValuesDependencies,
    DenseSet<Value *>& GlobalMemorySources) {
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] F:" << "\n";
    F->dump();
    );

    auto MemorySources = collectMemorySources(F);
    for (auto *GM : GlobalMemorySources) {
      MemorySources.insert(GM);
    }
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] Allocation Instructions: " << "\n";
    for (auto* I : MemorySources) {
        dbgs() << "\t";
        I->dump();
        dbgs() << "\tUsers: " << "\n";
        for (auto* U : I->users()) {
            dbgs() << "\t\t";
            U->dump();
        }
        dbgs() << "\n";
    }
    );

    // because we already analyzed real arguments for the F calls
    if (KnownRestrictFunctions.count(F)) {
        int i = 0;
        for (auto& Arg : F->args()) {
            if (KnownRestrictFunctions[F].count(i)) {
                MemorySources.insert(&Arg);
            }
            i++;
        }
    }

    auto TargetFunctionsCalls = collectCallsOfFunctionsWithPointerArguments(F);
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] TargetFunctionsCalls Instructions: " << "\n";
    for (auto* I : TargetFunctionsCalls) {
        dbgs() << "\t";
        I->dump();
    }
    );

    DenseMap<Function*, DenseSet<int>> LocallyAnalysedFunctions;
    for (auto* FCall : TargetFunctionsCalls) {
        auto* LF = FCall->getCalledFunction();
        LocallyAnalysedFunctions.insert(std::make_pair(LF, findPointerArgumentsIndexes(LF)));
    }

    auto CallsToPtrArgumentsMemorySources = fillCallsToPtrArgumentsMemorySources(
        TargetFunctionsCalls,
        MemorySources,
        FunctionsReturnValuesDependencies);
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] CallsToPtrArgumentsMemorySources: " << "\n";
    for (auto& CallInfo : CallsToPtrArgumentsMemorySources) {
        auto* CallI = CallInfo.second.mCallI;
        auto& ArgumentsSources = CallInfo.second.mArgumentMemorySources;
        dbgs() << "Call: ";
        CallI->dump();
        dbgs() << "Args memory sources:\n";
        for (auto& S : ArgumentsSources) {
            dbgs() << "\t" << S.first << ":\n";
            for (auto* MS : S.second) {
                dbgs() << "\t\t";
                MS->dump();
            }
        }
        dbgs() << "\n";
    }
    );

    auto RestrictFunctionsCallsByArgument = collectRestrictFunctionsCallsByArguments(
        CallsToPtrArgumentsMemorySources, FunctionsReturnValuesDependencies);
    auto LocalRestrictFunctionsInfo = collectRestrictFunctionsInfoByArguments(
        RestrictFunctionsCallsByArgument, TargetFunctionsCalls);

    DenseMap<Function*, DenseSet<int>> LocalNonRestrictFunctionsInfo;
    for (auto& AnalysedFunctionInfo : LocallyAnalysedFunctions) {
        auto* AnalysedFunction = AnalysedFunctionInfo.first;
        auto& PointerArgs = AnalysedFunctionInfo.second;
        // Function doesn't have any restrict argument
        if (!LocalRestrictFunctionsInfo.count(AnalysedFunction)) {
            LocalNonRestrictFunctionsInfo.insert(std::make_pair(AnalysedFunction, DenseSet<int>(PointerArgs)));
        }
        else {
            for (auto PointerArgIdx : PointerArgs) {
                // Argument isn't restrict
                if (!LocalRestrictFunctionsInfo[AnalysedFunction].count(PointerArgIdx)) {
                    if (LocalNonRestrictFunctionsInfo.count(AnalysedFunction)) {
                        LocalNonRestrictFunctionsInfo[AnalysedFunction].insert(PointerArgIdx);
                    }
                    else {
                        LocalNonRestrictFunctionsInfo.insert(std::make_pair(AnalysedFunction, DenseSet<int>({ PointerArgIdx })));
                    }
                }
            }
        }
    }

    for (auto& LRFInfo : LocalRestrictFunctionsInfo) {
        auto* LRF = LRFInfo.first;
        auto& FRestrictArgs = LRFInfo.second;
        if (KnownNonRestrictFunctions.count(LRF)) {
            for (auto LocalRestrictArg : FRestrictArgs) {
                if (!KnownNonRestrictFunctions[LRF].count(LocalRestrictArg)) {
                    if (KnownRestrictFunctions.count(LRF)) {
                        KnownRestrictFunctions[LRF].insert(LocalRestrictArg);
                    }
                    else {
                        KnownRestrictFunctions.insert(std::make_pair(LRF, DenseSet<int>({ LocalRestrictArg })));
                    }
                }
            }
        }
        else {
            if (KnownRestrictFunctions.count(LRF)) {
                KnownRestrictFunctions[LRF].insert(FRestrictArgs.begin(), FRestrictArgs.end());
            }
            else {
                KnownRestrictFunctions.insert(std::make_pair(LRF, DenseSet<int>(FRestrictArgs)));
            }
        }
    }

    for (auto& LNRFInfo : LocalNonRestrictFunctionsInfo) {
        auto* LNRF = LNRFInfo.first;
        auto& FNonRestrictArgs = LNRFInfo.second;
        if (KnownRestrictFunctions.count(LNRF)) {
            for (auto LocalNonRestrictArg : FNonRestrictArgs) {
                if (KnownRestrictFunctions[LNRF].count(LocalNonRestrictArg)) {
                    KnownRestrictFunctions[LNRF].erase(LocalNonRestrictArg);
                }
            }
            if (KnownRestrictFunctions[LNRF].empty()) {
                KnownNonRestrictFunctions.erase(LNRF);
            }
        }

        if (KnownNonRestrictFunctions.count(LNRF)) {
            KnownNonRestrictFunctions[LNRF].insert(FNonRestrictArgs.begin(), FNonRestrictArgs.end());
        }
        else {
            KnownNonRestrictFunctions.insert(std::make_pair(LNRF, DenseSet<int>(FNonRestrictArgs)));
        }
    }
}

FunctionResultArgumentsMemoryDependencies findReturnValueDependencies(Function* F, DenseMap<Function*,
    FunctionResultArgumentsMemoryDependencies>& FunctionsReturnValuesDependencies) {
    FunctionResultArgumentsMemoryDependencies dependencies;
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] F:" << "\n";
    F->dump();
    );
    if (F->getReturnType()->isPointerTy()) {
        int i = 0;
        for (auto& Arg : F->args()) {
            if (Arg.getType()->isPointerTy()) {
                SmallVector<Value*, 16> workList;
                workList.push_back(&Arg);
                while (!workList.empty()) {
                    auto* workValue = workList.back();
                    workList.pop_back();
                    for (auto* U : workValue->users()) {
                        if (auto* GepU = dyn_cast<GetElementPtrInst>(U)) {
                            workList.push_back(U);
                        }
                        if (auto* CastI = dyn_cast<CastInst>(U)) {
                            workList.push_back(U);
                        }
                        if (auto* SelectI = dyn_cast<SelectInst>(U)) {
                            workList.push_back(U);
                        }
                        if (auto* CallI = dyn_cast<CallInst>(U)) {
                            auto* CalledF = CallI->getCalledFunction();
                            if (FunctionsReturnValuesDependencies.count(CalledF) &&
                                !FunctionsReturnValuesDependencies[CalledF].mInfluencingArgumentsIndexes.empty()) {
                                for (int callArgIdx = 0; callArgIdx < CallI->getNumArgOperands(); callArgIdx++) {
                                    auto* CallArg = CallI->getArgOperand(callArgIdx);
                                    if (workValue == CallArg &&
                                        FunctionsReturnValuesDependencies[CalledF].mInfluencingArgumentsIndexes.count(callArgIdx)) {
                                        LLVM_DEBUG(
                                            dbgs() << "Inner call result depends on argument " << i << " callArgIdx: " << callArgIdx
                                            << " Call: ";
                                        CallI->dump();
                                        );
                                        workList.push_back(U);
                                    }
                                }
                            }
                        }
                        auto* RetI = dyn_cast<Instruction>(U);
                        if (RetI && RetI->isTerminator()) {
                            LLVM_DEBUG(
                                dbgs() << "Found Terminator inst: ";
                            RetI->dump();
                            );
                            dependencies.mInfluencingArgumentsIndexes.insert(i);
                        }
                    }
                }
            }
            i++;
        }
        // if result not depends on arguments, function may return unique memory for every call
        // (malloc wrapper as example) or may not (some static return)
        if (dependencies.mInfluencingArgumentsIndexes.empty()) {
            dependencies.mIsRestrict = F->returnDoesNotAlias();
            LLVM_DEBUG(
                dbgs() << "Function returnDoesNotAlias: " << F->returnDoesNotAlias() << "\n";
            );
        }
    }
    LLVM_DEBUG(
        dbgs() << "Function  " << F->getName() << " return depends on "
        << dependencies.mInfluencingArgumentsIndexes.size() << " args: " << "\n";
    for (auto Idx : dependencies.mInfluencingArgumentsIndexes) {
        dbgs() << "\t\t" << Idx << "\n";
    }
    dbgs() << "mIsRestrict:  " << dependencies.mIsRestrict
        << " returnDoesNotAlias: " << F->returnDoesNotAlias() << "\n";
    );
    return dependencies;
}

DenseMap<Function*, FunctionResultArgumentsMemoryDependencies> findFunctionsReturnValueDependencies(CallGraph& CG) {
    DenseMap<Function*, FunctionResultArgumentsMemoryDependencies> FunctionsReturnValuesDependencies;
    for (scc_iterator<CallGraph*> CallGraphIterator = scc_begin(&CG); !CallGraphIterator.isAtEnd(); ++CallGraphIterator) {
        const std::vector<CallGraphNode*>& NodeVec = *CallGraphIterator;
        if (NodeVec.size() > 1) {
            LLVM_DEBUG(
                dbgs() << "[RESTRICTION ARGS] Can't process recursion in the Call Graph. First func: \n";
            NodeVec[0]->dump();
            );
        }
        else {
            for (CallGraphNode* CGNode : *CallGraphIterator) {
                if (Function* F = CGNode->getFunction()) {
                    if (F->isIntrinsic())
                        continue;
                    auto dependencies = findReturnValueDependencies(F, FunctionsReturnValuesDependencies);
                    FunctionsReturnValuesDependencies.insert(std::make_pair(F, dependencies));
                }
            }
        }
    }
    return FunctionsReturnValuesDependencies;
}

bool RestrictionArgumentsPass::runOnModule(Module& M) {
    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS]" << "\n";
    M.dump();
    );

    DenseMap<Function*, DenseSet<int>> RestrictFunctionsInfo;
    DenseMap<Function*, DenseSet<int>> NonRestrictFunctionsInfo;

    auto& CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

    auto FunctionsReturnValuesDependencies = findFunctionsReturnValueDependencies(CG);
    std::vector<Function*> UpwardCalledFunctions;
    for (scc_iterator<CallGraph*> CallGraphIterator = scc_begin(&CG); !CallGraphIterator.isAtEnd(); ++CallGraphIterator) {
        const std::vector<CallGraphNode*>& NodeVec = *CallGraphIterator;
        if (NodeVec.size() > 1) {
            LLVM_DEBUG(
                dbgs() << "[RESTRICTION ARGS] Can't process recursion in the Call Graph. First func: \n";
            NodeVec[0]->dump();
            );
        }
        else {
            for (CallGraphNode* CGNode : *CallGraphIterator) {
                if (Function* F = CGNode->getFunction()) {
                    if (F->isIntrinsic())
                        continue;
                    UpwardCalledFunctions.push_back(F);
                }
            }
        }
    }

    DenseSet<Value *> GlobalMemorySources = collectGlobalMemorySources(&M);

    for (auto* F : llvm::reverse(UpwardCalledFunctions)) {
        runOnFunction(F, RestrictFunctionsInfo, NonRestrictFunctionsInfo, FunctionsReturnValuesDependencies, GlobalMemorySources);
    }

    LLVM_DEBUG(
        dbgs() << "[RESTRICTION ARGS] Restrict Functions: \n";
    for (auto& FInfo : RestrictFunctionsInfo) {
        auto F = FInfo.first;
        auto& Args = FInfo.second;
        dbgs() << "\t" << F->getName() << "\n";
        for (auto ArgIdx : Args) {
            dbgs() << "\t\t" << ArgIdx << "\n";
        }
    }

    dbgs() << "[RESTRICTION ARGS] Functions with Non Restrict calls:\n";
    for (auto& FInfo : NonRestrictFunctionsInfo) {
        auto F = FInfo.first;
        auto& Args = FInfo.second;
        dbgs() << "\t" << F->getName() << "\n";
        for (auto ArgIdx : Args) {
            dbgs() << "\t\t" << ArgIdx << "\n";
        }
    }

    dbgs() << "[RESTRICTION ARGS] Global vars:\n";
    for (auto& GV : GlobalMemorySources) {
        dbgs() << "\t" << GV->getName() << "\n";
        GV->dump();
    }
    );
 return false;
}

void RestrictionArgumentsPass::getAnalysisUsage(llvm::AnalysisUsage& AU) const {
    //AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<CallGraphWrapperPass>();
    AU.setPreservesAll();
}

ModulePass* llvm::createRestrictionArgumentsPass() { return new RestrictionArgumentsPass; }