//===--- Utils.cpp ------------ Memory Utils ---------------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Support/IRUtils.h"
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Dominators.h>

using namespace llvm;
using namespace tsar;

void tsar::findConstDbgUsers(
    SmallVectorImpl<DbgVariableIntrinsic *> &DbgUsers, Constant *C) {
  assert(C && "Value must not be null!");
  if (!C->isUsedByMetadata())
    return;
  if (auto *CMD = ConstantAsMetadata::getIfExists(C)) {
    if (auto *MDV = MetadataAsValue::getIfExists(C->getContext(), CMD))
      for (User *U : MDV->users())
        if (auto *DII{dyn_cast<DbgVariableIntrinsic>(U)})
          DbgUsers.push_back(DII);
    SmallPtrSet<DbgVariableIntrinsic *, 4> EncounteredDbgValues;
    for (Metadata *AL : CMD->getAllArgListUsers())
      if (auto *MDV = MetadataAsValue::getIfExists(C->getContext(), AL))
        for (User *U : MDV->users())
          if (DbgVariableIntrinsic *DII = dyn_cast<DbgVariableIntrinsic>(U))
            if (EncounteredDbgValues.insert(DII).second)
              DbgUsers.push_back(DII);
  }
}

void tsar::findAllDbgUsers(SmallVectorImpl<DbgVariableIntrinsic *> &DbgUsers,
                     Value *V) {
  if (auto *C{dyn_cast<Constant>(V)})
    findConstDbgUsers(DbgUsers, C);
  else
    findDbgUsers(DbgUsers, V);
}

namespace {
void findConstDbgValues(
    SmallVectorImpl<DbgValueInst *> &DbgValues, Constant *C) {
  assert(C && "Value must not be null!");
  if (auto *CMD = ConstantAsMetadata::getIfExists(C))
    if (auto *MDV = MetadataAsValue::getIfExists(C->getContext(), CMD))
      for (User *U : MDV->users())
        if (DbgValueInst *DVI = dyn_cast<DbgValueInst>(U))
          DbgValues.push_back(DVI);
}

void findAllDbgValues(SmallVectorImpl<DbgValueInst *> &DbgValues, Value *V) {
  if (auto *C = dyn_cast<Constant>(V))
    findConstDbgValues(DbgValues, C);
  else
    findDbgValues(DbgValues, V);
}

/// Computes a list of basic blocks which dominates a list of specified uses of
/// a value `V` and contains some of `llvm.dbg.value` which associate value
/// of `V` with a variable (not an address of a variable).
DenseMap<DIMemoryLocation, SmallPtrSet<BasicBlock *, 8>>
findDomDbgValues(const Value *V, const DominatorTree &DT,
    ArrayRef<Instruction *> Users) {
  SmallVector<DbgValueInst *, 1> AllDbgValues;
  findAllDbgValues(AllDbgValues, const_cast<Value *>(V));
  DenseMap<DIMemoryLocation, SmallPtrSet<BasicBlock *, 8>> Dominators;
  for (auto *DVI : AllDbgValues) {
    if (!DVI->getVariable() || hasDeref(*DVI->getExpression()))
      continue;
    bool IsDbgDom = true;
    auto UI = Users.begin(), UE = Users.end();
    for (; UI != UE; ++UI)
      if (!DT.dominates(DVI, *UI))
        break;
    if (UI != UE)
      continue;
    auto Pair = Dominators.try_emplace(DIMemoryLocation::get(DVI));
    Pair.first->second.insert(DVI->getParent());
  }
  return Dominators;
}

/// \brief Determines all basic blocks which contains metadata-level locations
/// aliases with a specified one.
///
/// Two conditions is going to be checked. At first it means that such block
/// should contain llvm.dbg.value intrinsic which is associated with a
/// specified metadata-level location `Loc` and a register other then `V`.
/// The second condition is that there is no llvm.dbg.values after this
/// intrinsic in the basic block which is associated with `Loc` and `V`.
SmallPtrSet<BasicBlock *, 16>
findAliasBlocks(const Value * V, const DIMemoryLocation &Loc) {
  assert(Loc.isValid() && "Location must be valid!");
  SmallPtrSet<BasicBlock *, 16> Aliases;
  auto *MDV = MetadataAsValue::getIfExists(
    Loc.Var->getContext(), const_cast<DIVariable *>(Loc.Var));
  if (!MDV)
    return Aliases;
  for (User *U : MDV->users()) {
    DbgValueInst *DVI = dyn_cast<DbgValueInst>(U);
    if (!DVI)
      continue;
    assert(DVI->getExpression() && "Expression must not be null!");
    if (hasDeref(*DVI->getExpression()) ||
        !mayAliasFragments(*DIMemoryLocation::get(DVI).Expr, *Loc.Expr))
      continue;
    auto I = DVI->getIterator(), E = DVI->getParent()->end();
    for (; I != E; ++I) {
      if (auto *SuccDVI = dyn_cast<DbgValueInst>(&*I))
        if (SuccDVI->getValue() == V && DIMemoryLocation::get(SuccDVI) == Loc) {
          break;
        }
    }
    if (I == E)
      Aliases.insert(DVI->getParent());
  }
  return Aliases;
}
}

namespace tsar {
std::pair<Value *, bool> GetUnderlyingObjectWithMetadata(
    Value *V, const DataLayout &DL) {
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    if (findGlobalMetadata(GV, DILocs))
      return std::make_pair(V, true);
  } else {
    if (V->isUsedByMetadata())
      return std::make_pair(V, true);
  }
  auto BasePtr = getUnderlyingObject(V, 1);
  return V == BasePtr ? std::make_pair(V, false) :
    GetUnderlyingObjectWithMetadata(BasePtr, DL);
}

DISubprogram *findMetadata(const Function *F) {
  assert(F && "Function must not be null!");
  if (auto *SP = F->getSubprogram())
    return SP;
  return dyn_cast_or_null<DISubprogram>(F->getMetadata("sapfor.dbg"));
}


bool findGlobalDIExpression(const GlobalObject *GO,
    SmallVectorImpl<DIGlobalVariableExpression *> &DIExprs) {
  assert(GO && "Global must not be null!");
  SmallVector<MDNode *, 1> MDs;
  GO->getMetadata(LLVMContext::MD_dbg, MDs);
  if (MDs.empty())
    GO->getMetadata("sapfor.dbg", MDs);
  bool IsChanged = false;
  for (auto *MD : MDs) {
    if (auto DIExpr = dyn_cast_or_null<DIGlobalVariableExpression>(MD)) {
      DIExprs.push_back(DIExpr);
      IsChanged = true;
    }
  }
  return IsChanged;
}

bool findGlobalMetadata(const GlobalVariable *Var,
    SmallVectorImpl<DIMemoryLocation> &DILocs) {
  assert(Var && "Variable must not be null!");
  SmallVector<MDNode *, 1> MDs;
  Var->getMetadata(LLVMContext::MD_dbg, MDs);
  if (MDs.empty())
    Var->getMetadata("sapfor.dbg", MDs);
  bool IsChanged = false;
  for (auto *MD : MDs) {
    if (auto DIExpr = dyn_cast_or_null<DIGlobalVariableExpression>(MD)) {
      auto *Expr = DIExpr->getExpression();
      if (auto Frag = Expr->getFragmentInfo()) {
        if (Expr->getNumElements() != 3)
          continue;
        Expr = DIExpression::get(
            Expr->getContext(),
            {dwarf::DW_OP_LLVM_fragment, Frag->OffsetInBits, Frag->SizeInBits});
      } else if (Expr->getNumElements() > 0) {
        continue;
      }
      DILocs.push_back(DIMemoryLocation::get(DIExpr->getVariable(),
                                             DIExpr->getExpression()));
      IsChanged = true;
    }
  }
  return IsChanged;
}

Optional<DIMemoryLocation> findMetadata(const Value *V,
    ArrayRef<Instruction *> Users,  const DominatorTree &DT,
    SmallVectorImpl<DIMemoryLocation> &DILocs) {
  auto DILocsOriginSize = DILocs.size();
  auto Dominators = findDomDbgValues(V, DT, Users);
  for (auto &DILocToDom : Dominators) {
    auto DILoc = DILocToDom.first;
    // If Aliases is empty it does not mean that a current variable can be used
    // because if there are usage of V between two llvm.dbg.value (the first is
    // associated with some other variable and the second is associated with the
    // DILoc) the appropriate basic block will not be inserted into Aliases.
    auto Aliases = findAliasBlocks(V, DILoc);
    // Checks that each definition of a variable `DILoc` in `DILocToDom.second`
    // reaches each uses of `V` in `Users`.
    bool IsReached = false;
    for (auto *I : Users) {
      auto BB = I->getParent();
      auto ReverseItr = I->getReverseIterator(), ReverseItrE = BB->rend();
      for (IsReached = false; ReverseItr != ReverseItrE; ++ReverseItr) {
        if (auto *DVI = dyn_cast<DbgValueInst>(&*ReverseItr))
          if (!hasDeref(*DVI->getExpression()) &&
              DIMemoryLocation::get(DVI) == DILoc) {
            if (DVI->getValue() == V)
              IsReached = true;
            break;
          }
      }
      if (IsReached)
        continue;
      if (ReverseItr != ReverseItrE)
        break;
      // A backward traverse of CFG will be performed. It is started in a `BB`.
      SmallPtrSet<BasicBlock *, 8> VisitedPreds;
      std::vector<BasicBlock *> Worklist;
      Worklist.push_back(BB);
      IsReached = true;
      while (!Worklist.empty()) {
        auto WorkBB = Worklist.back();
        Worklist.pop_back();
        auto PredItr = pred_begin(WorkBB), PredItrE = pred_end(WorkBB);
        for (; PredItr != PredItrE; ++PredItr) {
          if (VisitedPreds.count(*PredItr))
            continue;
          if (Aliases.count(*PredItr))
            break;
          if (DILocToDom.second.count(*PredItr))
            continue;
          Worklist.push_back(*PredItr);
          VisitedPreds.insert(*PredItr);
        }
        if (PredItr != PredItrE) {
          IsReached = false;
          break;
        }
      }
      if (!IsReached)
        break;
    }
    if (IsReached)
      DILocs.push_back(DILoc);
  }
  auto DILocItrB = DILocs.begin() + DILocsOriginSize, DILocItrE = DILocs.end();
  for (auto DILocItr = DILocItrB; DILocItr != DILocItrE; ++DILocItr) {
    if ([&V, &DILocs, &DILocItr, &DILocItrB, &DILocItrE, &Dominators, &DT]() {
      auto DILocToDom = Dominators.find(*DILocItr);
      for (auto I = DILocItrB, E = DILocItrE; I != E; ++I) {
        if (I == DILocItr)
          continue;
        for (auto BB : Dominators.find(*I)->second)
          for (auto DILocBB : DILocToDom->second)
            if (DILocBB == BB) {
              for (auto &Inst : *DILocBB)
                if (auto DVI = dyn_cast<DbgValueInst>(&Inst))
                  if (DVI->getValue() == V && !hasDeref(*DVI->getExpression()))
                    if (DIMemoryLocation::get(DVI) == *DILocItr)
                      break;
                    else if (DIMemoryLocation::get(DVI) == *I)
                      return false;
            } else if (!DT.dominates(DILocBB, BB)) {
              return false;
            }
      }
      return true;
    }())
      return *DILocItr;
  }
  return None;
}

Optional<DIMemoryLocation> findMetadata(const Value * V,
    SmallVectorImpl<DIMemoryLocation> &DILocs, const DominatorTree *DT,
    MDSearch MDS, MDSearch *Status) {
  assert(V && "Value must not be null!");
  DILocs.clear();
  if (Status)
    *Status = MDSearch::AddressOfVariable;
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    if ((MDS == MDSearch::Any || MDS == MDSearch::AddressOfVariable) &&
        findGlobalMetadata(GV, DILocs) && DILocs.size() == 1)
      return DILocs.back();
    else
      return None;
  }
  if ((MDS == MDSearch::Any || MDS == MDSearch::AddressOfVariable) &&
      !isa<Constant>(V)) {
    auto AddrUses = FindDbgAddrUses(const_cast<Value *>(V));
    if (!AddrUses.empty()) {
      auto DIVar = AddrUses.front()->getVariable();
      if (DIVar) {
        for (auto DbgI : AddrUses) {
          if (DbgI->getVariable() != DIVar ||
            DbgI->getExpression() != AddrUses.front()->getExpression())
            return None;
        }
        auto DIM = DIMemoryLocation::get(AddrUses.front());
        if (!DIM.isValid())
          return None;
        DILocs.push_back(std::move(DIM));
        return DILocs.back();
      }
      return None;
    }
  }
  if (!DT || MDS != MDSearch::Any && MDS != MDSearch::ValueOfVariable)
    return None;
  if (Status)
    *Status = MDSearch::ValueOfVariable;
  SmallVector<Instruction *, 8> Users;
  // TODO (kaniandr@gmail.com): User is not an `Instruction` sometimes.
  // If `V` is a declaration of a function then call of this function will
  // be a `ConstantExpr`. May be some other cases exist.
  for (User *U : const_cast<Value *>(V)->users()) {
    if (auto Phi = dyn_cast<PHINode>(U)) {
      if (auto I = dyn_cast<Instruction>(const_cast<Value *>(V)))
        if (auto BB = I->getParent()) {
          Users.push_back(&BB->back());
          continue;
        }
    }
    if (auto I = dyn_cast<Instruction>(U))
      Users.push_back(I);
    else
      return None;
  }
  return findMetadata(V, Users, *DT, DILocs);
}

bool hasDeref(const DIExpression &Expr) {
  for (auto I = Expr.expr_op_begin(), E = Expr.expr_op_end(); I != E; ++I)
    if (I->getOp() == dwarf::DW_OP_deref || I->getOp() == dwarf::DW_OP_xderef)
      return true;
  return false;
}


bool mayAliasFragments(const DIExpression &LHS, const DIExpression &RHS) {
  if (LHS.getNumElements() != 3 || RHS.getNumElements() != 3)
    return true;
  auto LHSFragment = LHS.getFragmentInfo();
  auto RHSFragment = RHS.getFragmentInfo();
  if (!LHSFragment || !RHSFragment)
    return true;
  if (LHSFragment->SizeInBits == 0 || RHSFragment->SizeInBits == 0)
    return false;
  return ((LHSFragment->OffsetInBits == RHSFragment->OffsetInBits) ||
          (LHSFragment->OffsetInBits < RHSFragment->OffsetInBits &&
          LHSFragment->OffsetInBits + LHSFragment->SizeInBits >
            RHSFragment->OffsetInBits) ||
          (RHSFragment->OffsetInBits < LHSFragment->OffsetInBits &&
          RHSFragment->OffsetInBits + RHSFragment->SizeInBits >
            LHSFragment->OffsetInBits));
}

bool isLoopInvariant(const SCEV *Expr, const Loop *L,
    TargetLibraryInfo &TLI, ScalarEvolution &SE, const DefUseSet &DUS,
    const AliasTree &AT, const SpanningTreeRelation<const AliasTree *> &STR) {
  assert(Expr && "Expression must not be null!");
  assert(L && "Loop must not be null!");
  if (isa<SCEVCouldNotCompute>(Expr))
    return false;
  if (!SE.isLoopInvariant(Expr, L)) {
    while (auto *Cast = dyn_cast<SCEVCastExpr>(Expr))
      Expr = Cast->getOperand();
    if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(Expr)) {
      if (!L || AddRec->getLoop() == L || L->contains(AddRec->getLoop()))
        return false;
      if (AddRec->getLoop()->contains(L))
        return true;
      for (auto *Op : AddRec->operands())
        if (!isLoopInvariant(Op, L, TLI, SE, DUS, AT, STR))
          return false;
    } else if (auto *NAry = dyn_cast<SCEVNAryExpr>(Expr)) {
      for (auto *Op : NAry->operands())
        if (!isLoopInvariant(Op, L, TLI, SE, DUS, AT, STR))
          return false;
    } else if (auto *UDiv = dyn_cast<SCEVUDivExpr>(Expr)) {
      return isLoopInvariant(UDiv->getLHS(), L, TLI, SE, DUS, AT, STR) &&
        isLoopInvariant(UDiv->getRHS(), L, TLI, SE, DUS, AT, STR);
    } else {
      if (auto *I = dyn_cast<Instruction>(cast<SCEVUnknown>(Expr)->getValue()))
        return (L && isLoopInvariant(*I, *L, TLI, DUS, AT, STR)) ||
               (!L && isFunctionInvariant(*I, TLI, DUS, AT, STR));
      return true;
    }
  }
  return true;
}
}

template<class FunctionT>
static inline bool isRegionInvariant(Instruction &I,
    TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR, FunctionT &&Contains) {
  if (!Contains(I))
    return true;
  if (isa<PHINode>(I))
    return false;
  if (!accessInvariantMemory(I, TLI, DUS, AT, STR))
    return false;
  for (auto &Op : I.operands()) {
    if (auto *I = dyn_cast<Instruction>(Op)) {
      if (!isRegionInvariant(*I, TLI, DUS, AT, STR, Contains))
        return false;
    }
  }
  return true;
}

namespace tsar {
bool isLoopInvariant(llvm::Instruction &I, const llvm::Loop &L,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [&L](const Instruction &I) {return L.contains(&I); });
}

bool isFunctionInvariant(llvm::Instruction &I,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [](const Instruction &) {return true; });
}

bool isBlockInvariant(llvm::Instruction &I, const BasicBlock &BB,
    llvm::TargetLibraryInfo &TLI, const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  return isRegionInvariant(I, TLI, DUS, AT, STR,
    [&BB](const Instruction &I) {return I.getParent() == &BB; });
}

bool accessInvariantMemory(Instruction &I, TargetLibraryInfo &TLI,
    const DefUseSet &DUS, const AliasTree &AT,
    const SpanningTreeRelation<const AliasTree *> &STR) {
  bool Result = true;
  for_each_memory(I, TLI, [&DUS, &Result](
      Instruction &, MemoryLocation &&Loc,
      unsigned Idx, AccessInfo R, AccessInfo W) {
    Result &= !(DUS.hasDef(Loc) || DUS.hasMayDef(Loc));
  }, [&DUS, &AT, &STR, &Result](Instruction &I, AccessInfo, AccessInfo) {
    if (!Result)
      return;
    if (auto *Call = dyn_cast<CallBase>(&I)) {
      if (AT.getAliasAnalysis().doesNotAccessMemory(Call))
        return;
      Result &= AT.getAliasAnalysis().onlyReadsMemory(Call);
    } else {
      Result = false;
    }
    if (!Result)
      return;
    auto *AN = AT.findUnknown(I);
    assert(AN && "Unknown memory access must be presented in alias tree!");
    for (auto &Loc : DUS.getExplicitAccesses()) {
      if (!DUS.hasDef(Loc) && !DUS.hasMayDef(Loc))
        continue;
      auto EM = AT.find(Loc);
      assert(EM && "Memory location must be presented in alias tree!");
      if (!STR.isUnreachable(EM->getAliasNode(AT), AN)) {
        Result = false;
        return;
      }
    }
    for (auto *Loc : DUS.getExplicitUnknowns()) {
      if (auto *Call = dyn_cast<CallBase>(Loc))
        if (AT.getAliasAnalysis().onlyReadsMemory(Call))
          continue;
      auto UN = AT.findUnknown(*Loc);
      assert(UN &&
        "Unknown memory location must be presented in alias tree!");
      if (!STR.isUnreachable(UN, AN)) {
        Result = false;
        return;
      }
    }
  });
  return Result;
}

std::pair<bool, bool> isPure(const llvm::Function &F, const DefUseSet &DUS) {
  if (!DUS.getExplicitUnknowns().empty())
    return std::pair{false, false};
  if (find_if(instructions(F), [](const Instruction &I) {
        return isa<IntToPtrInst>(I);
      }) != inst_end(F))
    return std::pair{false, false};
  for (auto &Arg : F.args())
    if (auto Ty = dyn_cast<PointerType>(Arg.getType())) {
      auto PointeeTy{getPointerElementType(Arg)};
      if (!PointeeTy || hasUnderlyingPointer(PointeeTy))
        return std::pair{false, false};
    }
  auto &DL = F.getParent()->getDataLayout();
  bool HasGlobalAccess{false};
  for (auto &Range : DUS.getExplicitAccesses()) {
    auto *Ptr{getUnderlyingObject(const_cast<Value *>(Range.Ptr), 0)};
    if (isa<GlobalValue>(Ptr))
      HasGlobalAccess = true;
    if (isa<GlobalValue>(stripPointer(DL, Ptr)))
      return std::pair{false, false};
  }
  return std::pair{!HasGlobalAccess, HasGlobalAccess};
}
}

template <> char GlobalsAAResultImmutableWrapper::ID = 0;
using namespace llvm;
INITIALIZE_PASS(GlobalsAAResultImmutableWrapper, "globals-aa-iw",
                "Globals Alias Analysis Wrapper", true, true)
