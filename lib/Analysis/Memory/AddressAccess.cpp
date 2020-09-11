#include "tsar/Analysis/Memory/PrivateAnalysis.h"
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
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
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
#include <queue>
#include <unordered_set>


#undef DEBUG_TYPE
#define DEBUG_TYPE "address-access"

using namespace llvm;
using namespace tsar;
using namespace tsar::detail;
using bcl::operator "" _b;

char AddressAccessAnalyser::ID = 0;

namespace {
    using FunctionPassesProvider = FunctionPassProvider<MemoryDependenceWrapperPass>;
}

INITIALIZE_PROVIDER_BEGIN(FunctionPassesProvider,
                          "function-passes-provider",
                          "MemoryDependenceWrapperPass provider")
INITIALIZE_PASS_DEPENDENCY(MemoryDependenceWrapperPass)
INITIALIZE_PROVIDER_END(FunctionPassesProvider, "function-passes-provider",
                        "MemoryDependenceWrapperPass provider")

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

void AddressAccessAnalyser::initDepInfo(llvm::Function *F) {
    // мапит инструкции на зависимые от них
    // например, такой код:
    // store %a, %a.addr
    // %1 = load %a.addr
    // должен породить такой словарь:
    // {'store %a, %a.addr': ['%1 = load %a.addr']}
    FunctionPassesProvider *Provider = &getAnalysis<FunctionPassesProvider>(*F);
    auto MDA = &Provider->get<MemoryDependenceWrapperPass>().getMemDep();

    if (mDepInfo) {
        for (auto pair : *mDepInfo)
            delete(pair.second);

        delete(mDepInfo);
    }

    mDepInfo = new DependentInfo();

    for (BasicBlock &BB : F->getBasicBlockList())
        for (Instruction &I: BB) {
            Instruction *Inst = &I;

            if (!Inst->mayReadFromMemory() && !Inst->mayWriteToMemory())
                continue;

            MemDepResult Res = MDA->getDependency(Inst);
            if (!Res.isNonLocal()) {
                Instruction *target = Res.getInst();
                if (mDepInfo->find(target) == mDepInfo->end())
                    (*mDepInfo)[target] = new InstrDependencies();

                if (!Res.isDef())
                    (*mDepInfo)[target]->is_invalid = true;
                else  // пока нужны только эти
                    (*mDepInfo)[target]->deps.push_back(&I);
            } else if (auto CS = CallSite(Inst)) {
                // все инструкции, от которых наша инструкция зависит вот таким безобразным образом
                // помечаем как невалидные
                const MemoryDependenceResults::NonLocalDepInfo &NLDI =
                        MDA->getNonLocalCallDependency(CS);

                for (const NonLocalDepEntry &NLDE : NLDI) {
                    const MemDepResult &Res = NLDE.getResult();
                    Instruction *target = Res.getInst();
                    if (mDepInfo->find(target) == mDepInfo->end())
                        (*mDepInfo)[target] = new InstrDependencies(true);
                    else
                        (*mDepInfo)[target]->is_invalid = true;
                }
            } else {
                // все инструкции, от которых наша инструкция зависит вот таким безобразным образом
                // помечаем как невалидные
                SmallVector<NonLocalDepResult, 4> NLDI;
                assert((isa<LoadInst>(Inst) || isa<StoreInst>(Inst) ||
                        isa<VAArgInst>(Inst)) && "Unknown memory instruction!");
                MDA->getNonLocalPointerDependency(Inst, NLDI);

                for (const NonLocalDepResult &NLDR : NLDI) {
                    const MemDepResult &Res = NLDR.getResult();

                    Instruction *target = Res.getInst();
                    if (mDepInfo->find(target) == mDepInfo->end())
                        (*mDepInfo)[target] = new InstrDependencies(true);
                    else
                        (*mDepInfo)[target]->is_invalid = true;
                }
            }
        }
}

bool AddressAccessAnalyser::isStored(llvm::Value *v) {
    for (auto user : v->users()) {
        if (auto store = dyn_cast<StoreInst>(user))
            if (store->getValueOperand() == v)
                return true;
    }

    return false;
}

std::vector<Value *> AddressAccessAnalyser::useMovesTo(Value *value, Instruction* instr) {

    auto res = std::vector<Value *>();
    if (auto store = dyn_cast<StoreInst>(instr)) {
        Value *storedTo = store->getPointerOperand();
        if (storedTo == value)  // сохранять что-то по адресу из аргумента -- ок
            return res;
        // если же мы сохраняем сам адрес, то ищем его load'ы и возвращаем их, чтобы проверить что они не "утекают"
        // но сначала проверяем, что сохраняем в безопасное место (результат alloca)
        if (!dyn_cast<AllocaInst>(storedTo)) {
            LLVM_DEBUG(errs() << "store to a suspicious address" << "\n";);
            throw MoveUndefined();
        }

        // и затем проверяем, что адрес, по которому сохранили сам нигде не сохраняется
        if (isStored(storedTo)) {
            LLVM_DEBUG(errs() << "store to address which is stored" << "\n";);
            throw MoveUndefined();
        }

        // а теперь ищем его load'ы
        auto deps = mDepInfo->find(store);
        if (deps == mDepInfo->end()) {
            LLVM_DEBUG(errs() << "store has no deps" << "\n";);
            return res;
        }
        if (deps->second->is_invalid)  {
            LLVM_DEBUG(errs() << "store has deps which we don't process" << "\n";);
            throw MoveUndefined();
        }
        for (auto &I : deps->second->deps)
            res.push_back(I);
        return res;
    } else if (auto load = dyn_cast<LoadInst>(instr)) {
        return res;  // загружать из адреса аргумента -- ок
    } else if (auto gep = dyn_cast<GetElementPtrInst>(instr)) {
        // результат gep становится алиасом нашего адреса
        // нужно проанализировать не "утекает" ли он
        res.push_back(gep);
        return res;
    } else if (auto call = dyn_cast<CallInst>(instr)) {
        Function* called = call->getCalledFunction();
        if (mParameterAccesses.infoByFun.find(called) == mParameterAccesses.infoByFun.end()) {
            LLVM_DEBUG(errs() << "called not processed yet" << "\n";);
            throw MoveUndefined();
        }
        int argIdx = 0;
        for (Use &arg_use : call->arg_operands()) {
            Value* arg_value = arg_use.get();
            if (arg_value == value) {  // проверяем что эта функция не сохраняет аргумент, которым является наш value
                if (mParameterAccesses.infoByFun[called]->find(argIdx) != mParameterAccesses.infoByFun[called]->end()) {
                    throw MoveUndefined();
                }
            }
            argIdx++;
        }
        return res;
    } else {
        // при любых других использованиях значения, считаем, что оно может "утечь"
        throw MoveUndefined();
    }
}

bool AddressAccessAnalyser::contructUsageTree(Argument *arg) {
    auto workList = std::queue<Value *>();
    workList.push(arg);
    auto seen = std::unordered_set<Value *>();  // так точно не зациклимся

    while (!workList.empty()) {
        Value* v = workList.front();
        workList.pop();

        // analyze users
        for (User *user : v->users()) {
            auto instr = dyn_cast<Instruction>(user);
            if (!instr) {
                LLVM_DEBUG(errs() << "usage is not an instruction" << "\n";);
                return false;
            }

            try {
                auto dsts = useMovesTo(v, instr);

                for (Value *dst : dsts) {
                    if (seen.find(dst) != seen.end()) {
                        LLVM_DEBUG(errs() << "met already seen instruction" << "\n";);
                        return false;
                    }

                    seen.insert(dst);
                    workList.push(dst);
                }
            } catch (MoveUndefined& e) {
                return false;
            }
        }
    }

    return true;
}

void AddressAccessAnalyser::runOnFunction(Function *F) {
    mParameterAccesses.addFunction(F);  // if argument is here => ptr held by it may be stored somewhere
    initDepInfo(F);

    int argIdx = 0;
    for (Argument *Arg = F->arg_begin(); Arg != F->arg_end(); Arg++) {
        if (Arg->getType()->isPointerTy()) {  // we're not interested in non-ptr arguments
            if (!contructUsageTree(Arg))  // if usage tree exists then we have only local usages
                mParameterAccesses.infoByFun[F]->insert(argIdx);
        }
        argIdx++;
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
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping external node" << "\n";);
            continue;
        }
        if (F->isIntrinsic()) {
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping intrinsic function " << F->getName().str()       << "\n";);
            continue;
        }
        if (hasFnAttr(*F, AttrKind::LibFunc)) {
            LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: skipping lib function " << F->getName().str() << "\n";);
            continue;
        }

        LLVM_DEBUG(dbgs() << "[AddressAccessAnalyser]: analyzing function " << F->getName().str() << "\n";);
        runOnFunction(F);
    }

    return false;
}

bool AddressAccessAnalyser::isNonTrivialPointerType(llvm::Type *Ty) {
    assert(Ty && "Type must not be null!");
    if (Ty->isPointerTy())
        return hasUnderlyingPointer(Ty->getPointerElementType());
    if (Ty->isArrayTy())
        return hasUnderlyingPointer(Ty->getArrayElementType());
    if (Ty->isVectorTy())
        return hasUnderlyingPointer(Ty->getVectorElementType());
    if (Ty->isStructTy())
        for (unsigned I = 0, EI = Ty->getStructNumElements(); I < EI; ++I)
            return hasUnderlyingPointer(Ty->getStructElementType(I));
    return false;
}

void AddressAccessAnalyser::print(raw_ostream &OS, const Module *) const {
    fflush(stderr);
    for (auto &pair: mParameterAccesses.infoByFun) {
        const Function *F = pair.first;
        DenseSet<int> *parAccesses = pair.second;

        printf("Function [%s]: ", F->getName().begin());
        for (int Arg: *parAccesses)
            printf("\t%s, ", F->arg_begin()[Arg].getName().begin());
        printf("\n");
    }
}

bool PreservedParametersInfo::isPreserved(ImmutableCallSite CS, Use* use) {
    const Function* F = CS.getCalledFunction();

    if (!CS.isArgOperand(use))
        return false;

    if (!F)  {
        LLVM_DEBUG(errs() << "not a direct call" << "\n";);
        return true;
    }

    if (infoByFun.find(F) == infoByFun.end()) {
        LLVM_DEBUG(errs() << "called not processed yet" << "\n";);
        return true;
    }

    return infoByFun[F]->find(CS.getArgumentNo(use)) != infoByFun[F]->end();
}
