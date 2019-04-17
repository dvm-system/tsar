//===--- tsar_utility.cpp - Utility Methods and Classes----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements methods declared in tsar_utility.h
//
//===----------------------------------------------------------------------===//

#include "tsar_utility.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Transforms/Utils/Local.h>
#include <regex>

using namespace llvm;
using namespace tsar;

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
/// a value `V` and contains some of `llvm.dbg.value` associated with `V`.
DenseMap<DIMemoryLocation, SmallPtrSet<BasicBlock *, 8>>
findDomDbgValues(const Value *V, const DominatorTree &DT,
    ArrayRef<Instruction *> Users) {
  SmallVector<DbgValueInst *, 1> AllDbgValues;
  findAllDbgValues(AllDbgValues, const_cast<Value *>(V));
  DenseMap<DIMemoryLocation, SmallPtrSet<BasicBlock *, 8>> Dominators;
  for (auto *DVI : AllDbgValues) {
    if (!DVI->getVariable())
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
findAliasBlocks(const Value * V, const DIMemoryLocation Loc) {
  SmallPtrSet<BasicBlock *, 16> Aliases;
  auto *MDV = MetadataAsValue::getIfExists(
    Loc.Var->getContext(), const_cast<DIVariable *>(Loc.Var));
  if (!MDV)
    return Aliases;
  for (User *U : MDV->users()) {
    DbgValueInst *DVI = dyn_cast<DbgValueInst>(U);
    if (!DVI)
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

Value * cloneChainImpl(Value *From, Instruction *BoundInst, DominatorTree *DT,
    std::size_t BeforeCloneIdx, SmallVectorImpl<Instruction *> &CloneList) {
  assert(From && "Cloned value must not be null!");
  if (auto *AI = dyn_cast<AllocaInst>(From)) {
    assert(&AI->getFunction()->getEntryBlock() == AI->getParent() &&
      "Alloca instructions must be presented in an entry block only!");
    return From;
  }
  auto *I = dyn_cast<Instruction>(From);
  if (!I)
    return From;
  if (BoundInst && DT && DT->dominates(I, BoundInst))
    return From;
  if (isa<PHINode>(I)) {
    for (auto Idx = BeforeCloneIdx + 1, IdxE = CloneList.size();
         Idx != IdxE; ++Idx)
      CloneList[Idx]->deleteValue();
    CloneList.resize(BeforeCloneIdx + 1);
    return nullptr;
  }
  auto *Clone = I->clone();
  CloneList.push_back(Clone);
  for (unsigned OpIdx = 0, OpIdxE = Clone->getNumOperands();
       OpIdx < OpIdxE; ++OpIdx) {
    auto *Op = Clone->getOperand(OpIdx);
    auto *OpClone =
      cloneChainImpl(Op, BoundInst, DT, BeforeCloneIdx, CloneList);
    if (!OpClone)
      return nullptr;
    if (OpClone != Op)
      Clone->setOperand(OpIdx, OpClone);
  }
  return Clone;
}
}

namespace tsar {
std::vector<StringRef> tokenize(StringRef Str, StringRef Pattern) {
  std::vector<StringRef> Tokens;
  std::regex Rgx(Pattern.data());
  std::cmatch Cm;
  if (!std::regex_search(Str.data(), Cm, Rgx))
    return Tokens;
  bool HasSubMatch = false;
  for (std::size_t I = 1; I < Cm.size(); ++I)
    if (Cm[I].matched) {
      HasSubMatch = true;
      Tokens.emplace_back(Cm[I].first, Cm[I].length());
    }
  if (!HasSubMatch)
    Tokens.emplace_back(Cm[0].first, Cm[0].length());
  while (std::regex_search(Cm[0].second, Cm, Rgx)) {
    bool HasSubMatch = false;
    for (std::size_t I = 1; I < Cm.size(); ++I)
      if (Cm[I].matched) {
        HasSubMatch = true;
        Tokens.emplace_back(Cm[I].first, Cm[I].length());
      }
    if (!HasSubMatch)
      Tokens.emplace_back(Cm[0].first, Cm[0].length());
  }
  return Tokens;
}

llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo) {
  auto ArgItr = F.arg_begin();
  auto ArgItrE = F.arg_end();
  for (std::size_t I = 0; ArgItr != ArgItrE && I <= ArgNo; ++I, ++ArgItr);
  return ArgItr != ArgItrE ? &*ArgItr : nullptr;
}

Optional<unsigned> getLanguage(const DIVariable &DIVar) {
  auto Scope = DIVar.getScope();
  while (Scope) {
    auto *CU = isa<DISubprogram>(Scope) ?
      cast<DISubprogram>(Scope)->getUnit() : dyn_cast<DICompileUnit>(Scope);
    if (CU)
      return CU->getSourceLanguage();
    Scope = Scope->getScope().resolve();
  }
  return None;
}

bool cloneChain(Instruction *From,
    SmallVectorImpl<Instruction *> &CloneList,
    Instruction *BoundInst, DominatorTree *DT) {
  return cloneChainImpl(From, BoundInst, DT, CloneList.size(), CloneList);
}

bool findNotDom(Instruction *From, Instruction *BoundInst, DominatorTree *DT,
    SmallVectorImpl<Use *> &NotDom) {
  assert(From && "Instruction must not be null!");
  assert(BoundInst && "Bound instruction must not be null!");
  assert(DT && "Dominator tree must not be null!");
  if (!DT->dominates(From, BoundInst))
    return true;
  for (auto &Op : From->operands())
    if (auto *I = dyn_cast<Instruction>(&Op))
      if (findNotDom(I, BoundInst, DT, NotDom))
        NotDom.push_back(&Op);
  return false;
}

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
  auto BasePtr = GetUnderlyingObject(V, DL, 1);
  return V == BasePtr ? std::make_pair(V, false) :
    GetUnderlyingObjectWithMetadata(BasePtr, DL);
}

DISubprogram *findMetadata(const Function *F) {
  assert(F && "Function must not be null!");
  if (auto *SP = F->getSubprogram())
    return SP;
  return dyn_cast_or_null<DISubprogram>(F->getMetadata("sapfor.dbg"));
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
      DILocs.emplace_back(DIExpr->getVariable(), DIExpr->getExpression());
      IsChanged = true;
    }
  }
  return IsChanged;
}

Optional<DIMemoryLocation> findMetadata(const Value *V,
    ArrayRef<Instruction *> Users,  const DominatorTree &DT,
    SmallVectorImpl<DIMemoryLocation> &DILocs) {
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
          if (DIMemoryLocation::get(DVI) == DILoc) {
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
  auto DILocItr = DILocs.begin(), DILocItrE = DILocs.end();
  for (; DILocItr != DILocItrE; ++DILocItr) {
    if ([&V, &DILocs, &DILocItr, &DILocItrE, &Dominators, &DT]() {
      auto DILocToDom = Dominators.find(*DILocItr);
      for (auto I = DILocs.begin(), E = DILocItrE; I != E; ++I) {
        if (I == DILocItr)
          continue;
        for (auto BB : Dominators.find(*I)->second)
          for (auto DILocBB : DILocToDom->second)
            if (DILocBB == BB) {
              for (auto &Inst : *DILocBB)
                if (auto DVI = dyn_cast<DbgValueInst>(&Inst))
                  if (DVI->getValue() == V)
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
    MDSearch MDS) {
  assert(V && "Value must not be null!");
  DILocs.clear();
  if (MDS == MDSearch::Any || MDS == MDSearch::AddressOfVariable) {
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
  if (MDS != MDSearch::Any && MDS != MDSearch::ValueOfVariable)
    return None;
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    if (findGlobalMetadata(GV, DILocs) && DILocs.size() == 1)
      return DILocs.back();
    else
      return None;
  }
  if (!DT)
    return None;
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
}
