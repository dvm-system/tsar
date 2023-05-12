//=== ArrayScalarizer.cpp - Promote small arrays to pointer C++ *-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements loop pass that tries to promote small 
// one-dimensional array allocations to pointers.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include <tsar/Analysis/Memory/Utils.h>
#include <tsar/Unparse/Utils.h>
#include <tsar/Analysis/Memory/DependenceAnalysis.h>
#include "tsar/Support/IRUtils.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DIBuilder.h>
#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include <map>
#include <vector>
#include <string>
#include "llvm/IR/IRBuilder.h"
#include <llvm/Analysis/LoopPass.h>
#include <llvm/IR/Metadata.h>
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace tsar;
using namespace llvm;

namespace {

const int MAX_ARRAY_SIZE = 5;

/*
    Reduction operation structure:

    %addr = gep ...

    %load = load %addr
    %op = binOp %load, %expr

    store %op, %addr
*/
struct ReductionInfo {
    Value *expr;

    GetElementPtrInst *addr;    // %addr = gep ...
    LoadInst *load;             // %load = load %addr
    BinaryOperator *op;         // %op = add %load, %expr
    StoreInst *store;           // store %op, %addr
};


class ArrayScalarizerPass : public LoopPass, private bcl::Uncopyable {
public:
    static char ID;

    ArrayScalarizerPass() : LoopPass(ID) {
        initializeArrayScalarizerPassPass(*PassRegistry::getPassRegistry());
    }

    bool runOnLoop(Loop *L, LPPassManager &LPM) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<EstimateMemoryPass>();
        AU.addRequired<DependenceAnalysisWrapperPass>();
    }

private:
    unsigned int loop_id = 0;

    std::set<AllocaInst*> allocas;
    void findAllSmallArrays(Loop &L);

    bool makeTransformation(Loop *L, AllocaInst *AI);

    std::vector<Value *> insertPrologue(Loop *L, AllocaInst *AI);
    void insertEpilogue(Loop *L, AllocaInst *AI, const std::vector<Value *> &values);
    void insertEpilogueToBB(Loop *L, AllocaInst *AI, BasicBlock *BB, const std::vector<Value *> &values);

    void replaceReductionWithSwitch(Loop *L, AllocaInst *AI, ReductionInfo info, const std::vector<Value *> &values);

    int getAllocatedArraySize(AllocaInst *AI);
    llvm::Type* getAllocatedArrayElementType(AllocaInst *AI);
    
    template<typename T>
    AllocaInst* getAllocaFromLoadOrStore(Instruction *instr) {
        if (auto *I = dyn_cast<T>(instr)) {
            if (auto *AI = dyn_cast<AllocaInst>(I->getPointerOperand())) {
                return AI;
            }

            if (auto *GEP = dyn_cast<GetElementPtrInst>(I->getPointerOperand())) {
                return dyn_cast<AllocaInst>(GEP->getPointerOperand());
            }
        }

        return nullptr;
    }

    template<typename T>
    bool isMemorySafe(AllocaInst *AI, T *I) {
        auto *EM1 = AT->find(MemoryLocation(AI, 12));
        auto *EM2 = AT->find(MemoryLocation::get(I));

        SpanningTreeRelation<AliasTree *> STR(AT);
        if (EM1 != nullptr && EM2 != nullptr) {
            return STR.isUnreachable(EM1->getAliasNode(*AT), EM2->getAliasNode(*AT));
        }

        return true;
    }

    AllocaInst* getAllocaFromLoadOrStore(Instruction *instr);
    DbgDeclareInst* getDbgDeclare(Instruction *I);
    Value* getLoadStoreIndex(Instruction *I);
    bool getReductionInfo(StoreInst *SI, ReductionInfo &info);
    bool isTransformationValid(Loop *L, AllocaInst *AI, ReductionInfo &info);
    std::vector<ReductionInfo> getReductions(Loop *L, AllocaInst *AI);


    AliasTree *AT;
    DependenceInfo *depInfo;
};

char ArrayScalarizerPass::ID = 0;

};

INITIALIZE_PASS_BEGIN(ArrayScalarizerPass, "array2reg",
    "Promote Array To Register", false, false)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_END(ArrayScalarizerPass, "array2reg",
    "Promote Array To Register", false, false)

LoopPass * llvm::createArrayScalarizerPass() {
    return new ArrayScalarizerPass();
}

bool ArrayScalarizerPass::runOnLoop(Loop *L, LPPassManager &LPM) {
    AT = &getAnalysis<EstimateMemoryPass>().getAliasTree();
    depInfo = &getAnalysis<DependenceAnalysisWrapperPass>().getDI();

    findAllSmallArrays(*L);

    bool changed = false;
    for (auto &alloca: allocas) {
        changed = changed || makeTransformation(L, alloca);
    }

    errs() << "========== M ===========\n";

    errs() << *L->getHeader()->getParent()->getParent();

    errs() << "========================\n";

    loop_id++;

    return changed;
}

int ArrayScalarizerPass::getAllocatedArraySize(AllocaInst *AI) {
    if (auto *AT = dyn_cast<ArrayType>(AI->getAllocatedType())) {
        return AT->getNumElements();
    }

    return -1;
}

llvm::Type* ArrayScalarizerPass::getAllocatedArrayElementType(AllocaInst *AI) {
    if (auto *AT = dyn_cast<ArrayType>(AI->getAllocatedType())) {
        return AT->getElementType();
    }

    return nullptr;
}

AllocaInst* ArrayScalarizerPass::getAllocaFromLoadOrStore(Instruction *instr) {
    if (auto *res = getAllocaFromLoadOrStore<LoadInst>(instr)) { return res; }
    if (auto *res = getAllocaFromLoadOrStore<StoreInst>(instr)) { return res; }

    return nullptr;
}

DbgDeclareInst* ArrayScalarizerPass::getDbgDeclare(Instruction *I) {
    auto *F = I->getParent()->getParent();

    for (auto &BB : *F) {
        for (auto &LocI : BB) {
            if (auto *DDI = dyn_cast<DbgDeclareInst>(&LocI)) {
                if (DDI->getAddress() == I) {
                    return DDI;
                }
            }
        }
    }

    return nullptr;
}

void ArrayScalarizerPass::findAllSmallArrays(Loop &L) {
    for (auto *BB: L.getBlocks()) {
        for (auto &I: *BB) {
            if (auto *AI = getAllocaFromLoadOrStore(&I)) {
                int size = getAllocatedArraySize(AI);
                if (size != -1 && size <= MAX_ARRAY_SIZE && allocas.find(AI) == allocas.end()) {
                    allocas.insert(AI);
                }
            }
        }
    }
}

Value* ArrayScalarizerPass::getLoadStoreIndex(Instruction *I) {
    if (auto *LI = dyn_cast<LoadInst>(I)) {
        auto *GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand());
        if (GEP) {
            return dyn_cast<Value>(GEP->getOperand(2));
        }
    }

    if (auto *SI = dyn_cast<StoreInst>(I)) {
        auto *GEP = dyn_cast<GetElementPtrInst>(SI->getPointerOperand());
        if (GEP) {
            return dyn_cast<Value>(GEP->getOperand(2));
        }
    }

    return nullptr;
}

bool ArrayScalarizerPass::getReductionInfo(StoreInst *SI, ReductionInfo &info) {
    info.store = SI;

    info.addr = dyn_cast<GetElementPtrInst>(SI->getPointerOperand());
    if (!info.addr) {
        return false;
    }

    info.op = dyn_cast<BinaryOperator>(SI->getOperand(0));
    if (!info.op) {
        return false;
    }

    info.load = dyn_cast<LoadInst>(info.op->getOperand(0));
    if (info.load && info.load->getPointerOperand() == info.addr) {
        info.expr = info.op->getOperand(1);
    } else {
        info.load = dyn_cast<LoadInst>(info.op->getOperand(1));
        if (!info.load) {
            return false;
        }

        info.expr = info.op->getOperand(0);
    }

    auto *GEP = dyn_cast<GetElementPtrInst>(info.load->getPointerOperand());
    if (!GEP || info.addr != GEP) {
        return false;
    }

    return true;
}

std::vector<ReductionInfo> ArrayScalarizerPass::getReductions(Loop *L, AllocaInst *AI) {
    std::vector<ReductionInfo> result;

    std::set<StoreInst *> processedStores;
    std::set<LoadInst *> processedLoads;

    for (auto *BB: L->getBlocks()) {
        for (auto &I: *BB) {
            if (auto *SI = dyn_cast<StoreInst>(&I)) {
                ReductionInfo info;
                if (getAllocaFromLoadOrStore(SI) == AI) {
                    if (getReductionInfo(SI, info)) {
                        result.push_back(info);

                        processedStores.insert(info.store);
                        processedLoads.insert(info.load);
                    }
                }
            }
        }
    }

    for (auto *BB: L->getBlocks()) {
        for (auto &I: *BB) {
            if (auto *SI = dyn_cast<StoreInst>(&I)) {
                if (processedStores.find(SI) == processedStores.end() && !isMemorySafe(AI, SI)) {
                    return std::vector<ReductionInfo>();
                }
            }
            if (auto *LI = dyn_cast<LoadInst>(&I)) {
                if (processedLoads.find(LI) == processedLoads.end() && !isMemorySafe(AI, LI)) {
                    return std::vector<ReductionInfo>();
                }
            }
        }
    }

    return result;
}

bool ArrayScalarizerPass::makeTransformation(Loop *L, AllocaInst *AI) {
    if (L->getParentLoop()) {
        return false;
    }

    auto reductions = getReductions(L, AI);
    if (reductions.size() < 1) {
        return false;
    }
    
    auto values = insertPrologue(L, AI);
    insertEpilogue(L, AI, values);

    for (auto reduction: reductions) {
        replaceReductionWithSwitch(L, AI, reduction, values);
    }

    return true;
}

std::vector<Value *> ArrayScalarizerPass::insertPrologue(Loop *L, AllocaInst *AI) {
    LLVMContext &context = L->getHeader()->getContext();

    auto *PH = L->getLoopPreheader();
    Function *F = PH->getParent();

    auto I = PH->end();
    --I;

    auto *pBB = PH->splitBasicBlock(I);

    IRBuilder<> builder(pBB);
    builder.SetInsertPoint(pBB->getFirstNonPHI());

    auto *declare = getDbgDeclare(AI);
    std::vector<Value *> values;

    DIBuilder DIB(*L->getHeader()->getModule());

    auto suf = std::to_string(loop_id);

    int size = getAllocatedArraySize(AI);

    for (int i = 0; i < size; ++i) {
        Value* indices[] = {ConstantInt::get(Type::getInt64Ty(context), 0), ConstantInt::get(Type::getInt64Ty(context), i)};
        auto *gep = builder.CreateGEP(AI->getAllocatedType(), AI, indices);
        auto *load = builder.CreateLoad(getAllocatedArrayElementType(AI), gep);
        auto *newVal = builder.CreateAlloca(getAllocatedArrayElementType(AI), nullptr);
        auto *store = builder.CreateStore(load, newVal);

        values.push_back(newVal);

        auto *arrayVar = declare->getVariable();

        std::string name = arrayVar->getName().str() + "[" + std::to_string(i) + "]_" + suf;

        DIBasicType *BT = DIB.createBasicType("int", 32, dwarf::DW_ATE_signed);

        DILocalVariable *D = DIB.createAutoVariable(
            arrayVar->getScope(), name, arrayVar->getFile(), arrayVar->getLine(), BT
        );

        DIB.insertDeclare(newVal, D, declare->getExpression(), declare->getDebugLoc(), store);
    }

    return values;
}

void ArrayScalarizerPass::insertEpilogue(Loop *L, AllocaInst *AI, const std::vector<Value *> &values) {
    SmallVector<BasicBlock *> ExitBlocks(0);
    L->getExitBlocks(ExitBlocks);
    for (auto *BB: ExitBlocks) {
        insertEpilogueToBB(L, AI, BB, values);
    }
}

void ArrayScalarizerPass::insertEpilogueToBB(Loop *L, AllocaInst *AI, BasicBlock *BB, const std::vector<Value *> &values) {
    LLVMContext &context = BB->getContext();
    Function *F = BB->getParent();

    IRBuilder<> builder(BB);
    builder.SetInsertPoint(BB->getFirstNonPHI());

    auto *declare = getDbgDeclare(AI);

    int size = getAllocatedArraySize(AI);
    for (int i = 0; i < size; ++i) {
        Value* indices[] = {ConstantInt::get(Type::getInt64Ty(context), 0), ConstantInt::get(Type::getInt64Ty(context), i)};
        auto *gep = builder.CreateGEP(AI->getAllocatedType(), AI, indices, "element", true);
        auto *val = builder.CreateLoad(getAllocatedArrayElementType(AI), values[i]);

        auto *store = builder.CreateStore(val, gep);
    }
}

void ArrayScalarizerPass::replaceReductionWithSwitch(Loop *L, AllocaInst *AI, ReductionInfo info, const std::vector<Value *> &values) {
    auto &context = L->getHeader()->getContext();
    auto *F = L->getHeader()->getParent();
    auto *BB = info.store->getParent();

    auto *pBB = BB->splitBasicBlock(info.store);

    auto *CI = getLoadStoreIndex(info.store);

    auto *switchInst = SwitchInst::Create(CI, pBB, values.size());
    ReplaceInstWithInst(BB->getTerminator(), switchInst);

    for (int i = 0; i < values.size(); ++i) {
        auto *caseBB = BasicBlock::Create(context, "case", F, BB);
        IRBuilder<> caseBuilder(caseBB);


        auto *load = caseBuilder.CreateLoad(getAllocatedArrayElementType(AI), values[i]);

        auto *newAdd = caseBuilder.CreateBinOp(info.op->getOpcode(), info.expr, load);
        auto *store = caseBuilder.CreateStore(newAdd, values[i]);

        auto *br = caseBuilder.CreateBr(pBB);

        switchInst->addCase(ConstantInt::get(Type::getInt64Ty(context), i), caseBB);
    }

    info.store->eraseFromParent();
    cast<Instruction>(info.op)->eraseFromParent();
    info.load->eraseFromParent();

    info.addr->eraseFromParent();
}