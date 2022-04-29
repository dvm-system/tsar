//===- DIArrayAccess.cpp - Array Access Collection (Metadata) ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a pass to collect array accesses in a program.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/PassProvider.h"
#include "tsar/Support/SCEVUtils.h"
#include "tsar/Unparse/Utils.h"
#include <bcl/utility.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/Analysis/IVDescriptors.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

#define DEBUG_TYPE "di-array-access"

void DIAffineSubscript::print(raw_ostream &OS) const {
  auto printSymbol = [&OS](const Symbol &S) {
    OS << S.Constant;
    if (S.Kind != Symbol::SK_Constant) {
      if (S.Variable) {
        OS << " + ";
        printDILocationSource(dwarf::DW_LANG_C, *S.Variable, OS);
      } else {
        OS << " + ?";
      }
      if (S.Kind == Symbol::SK_Induction)
        OS << "@I";
    }
  };
  printSymbol(getSymbol());
  for (unsigned I = 0, EI = getNumberOfMonoms(); I < EI; ++I) {
    uint64_t ID = 0;
    for (unsigned J = 0, EI = getMonom(I).Column->getNumOperands(); J < EI; ++J)
      if (auto *L = dyn_cast<DILocation>(getMonom(I).Column->getOperand(J))) {
        bcl::shrinkPair(L->getLine(), L->getColumn(), ID);
        break;
      }
    OS << " + ";
    printSymbol(getMonom(I).Value);
    OS << "*L" << ID;
  }
}

void DIArrayAccessInfo::add(DIArrayAccess *Access, ArrayRef<Scope> Scopes) {
  assert(Access && "Access must not be null!");
  auto A = Access->getArray();
  assert(A && "Array for access must be specified!");
  assert(Access->getParent() == Scopes.front() &&
         "The first scope must explicitly contain an access!");
  mArrays.insert(DIArrayHandle(A, this));
  // Search for innermost scope which is already presented in the list of scopes
  // and which contains a specified access.
  auto ParentItr = mScopeToAccesses.end(); // description of the found scope
  auto NestItr = Scopes.begin(); // reference to the found scope in the nest
  for (auto EI = Scopes.end(); NestItr != EI; ++NestItr) {
    ParentItr = mScopeToAccesses.find(*NestItr);
    if (ParentItr != mScopeToAccesses.end())
      break;
  }
  auto addToArrayAccesses = [this](DIArrayAccess *Access) -> array_iterator {
    auto ArrayInfo = mArrayToAccesses.try_emplace(Access->getArray());
    if (ArrayInfo.second) {
      ArrayInfo.first->get<End>() = mArrayAccesses.begin();
      mArrayAccesses.push_front(*Access);
      ArrayInfo.first->get<Begin>() = array_iterator(Access);
    } else {
      mArrayAccesses.insert(ArrayInfo.first->get<End>(), *Access);
      auto PrevArrayItr = array_iterator(Access);
      --PrevArrayItr;
      auto CurrScope = PrevArrayItr->getParent();
      for (;;) {
        auto ScopeItr = mScopeToAccesses.find(CurrScope);
        ScopeItr->get<ArrayToAccessMap>().find(Access->getArray())->get<End>() =
            array_iterator(Access);
        if (!ScopeItr->get<Parent>())
          break;
        CurrScope = ScopeItr->get<Parent>();
      }
    }
    return array_iterator(Access);
  };
  if (ParentItr == mScopeToAccesses.end()) {
    mAccesses.push_front(Access);
    auto InScopeItr = iterator(Access);
    auto InArrayItr = addToArrayAccesses(Access);
    // Any scope in the nest contains only current access. So the next access
    // in the list of accesses becomes a boundary of the list of scope accesses.
    // And the next access in the list of accesses sorted according to
    // descriptions of arrays becomes a boundary of the list of scope accesses
    // to the current array.
    auto EndScopeItr = InScopeItr;
    ++EndScopeItr;
    auto NextArrayItr = InArrayItr;
    ++NextArrayItr;
    Scope OuterScope{nullptr};
    for (auto &P : reverse(Scopes)) {
      auto I = mScopeToAccesses.try_emplace(P).first;
      I->get<Parent>() = OuterScope;
      I->get<Begin>() = InScopeItr;
      I->get<End>() = EndScopeItr;
      I->get<ArrayToAccessMap>().try_emplace(Access->getArray(), InArrayItr,
                                             NextArrayItr);
      OuterScope = P;
    }
  } else {
    mAccesses.insert(ParentItr->get<End>(), Access);
    auto InScopeItr = iterator(Access);
    SmallVector<ScopeToAccessMap::iterator, 4> VisitedScopes;
    auto OuterArrayItr = findOuterScopeWithArrayAccess(
        Access->getArray(), ParentItr, VisitedScopes);
    auto InArrayItr = mArrayAccesses.end();
    if (OuterArrayItr == VisitedScopes.back()->get<ArrayToAccessMap>().end()) {
      // There are no array accesses to the current array in the nest of scopes
      // 'Scopes'.
      InArrayItr = addToArrayAccesses(Access);
    } else {
      mArrayAccesses.insert(OuterArrayItr->get<End>(), *Access);
      InArrayItr = array_iterator(Access);
      // Update ranges of array accesses in scopes which do not contain the
      // current access and which are contained in the updated scope.
      auto ParentItr = VisitedScopes.pop_back_val();
      for (auto I = OuterArrayItr->get<Begin>(); I != InArrayItr; ++I) {
        auto CurrScope = I->getParent();
        while (CurrScope && CurrScope != ParentItr->get<Scope>()) {
          auto ScopeItr = mScopeToAccesses.find(CurrScope);
          assert(ScopeItr != mScopeToAccesses.end() && "Scope must be known!");
          auto ArrayItr =
              ScopeItr->get<ArrayToAccessMap>().find(Access->getArray());
          assert(ArrayItr != ScopeItr->get<ArrayToAccessMap>().end() &&
                 "Range of array accesses must be known!");
          if (ArrayItr->get<End>() == OuterArrayItr->get<End>())
            ArrayItr->get<End>() = InArrayItr;
          CurrScope = ScopeItr->get<Parent>();
        }
      }
    }
    auto NextArrayItr = InArrayItr;
    ++NextArrayItr;
    // Update list of array accesses (references to the nodes in list of
    // accesses which is sorted according to descriptions of arrays) for
    // scopes which did not previously contain accesses to the current array.
    for (auto &ScopeItr : VisitedScopes)
      ScopeItr->get<ArrayToAccessMap>().try_emplace(
          Access->getArray(), InArrayItr, NextArrayItr).second;
    // Now we update the range of accesses for scopes which are contained in
    // scope which were updated (ParentItr->get<Scope>()) if these scopes
    // do not contain new access.
    // B1  B2,B3   E3    New  E1,E2
    // x1 -- x2 -- x3 -- x4 -- x5
    // We insert new access x4 before x5 in scope 1: [B1, E1).
    // Access x2 is implicitly contained in scope 2: [B2, E2) which is
    // a parent for scope 3: [B3. E3). The right bounds for scopes 1 and 2 are
    // equal. If new access x4 is not contained in the scope 2 the right bound
    // E2 must be updated. It has to be equal to x4 to exclude x4 from
    // the scope 2.
    for (auto I = ParentItr->get<Begin>(); I != InScopeItr; ++I) {
      auto CurrScope = I->getParent();
      while (CurrScope && CurrScope != ParentItr->get<Scope>()) {
        auto ScopeItr = mScopeToAccesses.find(CurrScope);
        assert(ScopeItr != mScopeToAccesses.end() && "Scope must be known!");
        if (ScopeItr->get<End>() == ParentItr->get<End>())
          ScopeItr->get<End>() = InScopeItr;
        CurrScope = ScopeItr->get<Parent>();
      }
    }
    // Add new scopes to the collection of scopes.
    // Attention, the loop bellow invalidate ParentItr because it updates
    // mScopeToAccesses map.
    auto EndScopeItr = InScopeItr;
    ++EndScopeItr;
    for (auto OuterLoopItr = NestItr, LoopItrE = Scopes.begin();
         OuterLoopItr != LoopItrE; --OuterLoopItr) {
      auto I = mScopeToAccesses.try_emplace(*(OuterLoopItr - 1)).first;
      I->get<Parent>() = *OuterLoopItr;
      I->get<Begin>() = InScopeItr;
      I->get<End>() = EndScopeItr;
      I->get<ArrayToAccessMap>().try_emplace(Access->getArray(), InArrayItr,
                                             NextArrayItr);
    }
  }
}

void DIArrayAccessInfo::erase(const Array &V) {
  auto I = mArrayToAccesses.find(V);
  if (I == mArrayToAccesses.end())
    return;
  mArrays.erase(V);
  llvm::SmallVector<Scope, 8> ScopeToRemove;
  for (auto &Info : mScopeToAccesses) {
    auto BeginItr = Info.get<Begin>();
    auto EndItr = Info.get<End>();
    for (; BeginItr->getArray() == V && BeginItr != EndItr; ++BeginItr)
      ;
    if (BeginItr == EndItr) {
      ScopeToRemove.push_back(Info.get<Scope>());
      break;
    }
    for (; EndItr != mAccesses.end() && EndItr->getArray() == V; ++EndItr)
      ;
    Info.get<Begin>() = BeginItr;
    Info.get<End>() = EndItr;
    if (I->get<Begin>() != mArrayAccesses.begin()) {
      auto PrevItr = I->get<Begin>();
      --PrevItr;
      auto PrevArrayItr =
          Info.get<ArrayToAccessMap>().find(PrevItr->getArray());
      if (PrevArrayItr != Info.get<ArrayToAccessMap>().end() &&
          PrevArrayItr->get<End>()->getArray() == V)
        PrevArrayItr->get<End>() = I->get<End>();
    }
    Info.get<ArrayToAccessMap>().erase(V);
  }
  for (auto &S : ScopeToRemove)
    mScopeToAccesses.erase(S);
  if (I->get<Begin>() != mArrayAccesses.begin()) {
    auto PrevItr = I->get<Begin>();
    --PrevItr;
    auto PrevArrayItr = mArrayToAccesses.find(PrevItr->getArray());
    if (PrevArrayItr != mArrayToAccesses.end())
      PrevArrayItr->get<End>() = I->get<End>();
  }
  for (auto CurrItr = I->get<Begin>(); CurrItr != I->get<End>();) {
    auto &Access = *CurrItr;
    CurrItr = mArrayAccesses.erase(CurrItr);
    mAccesses.erase(Access);
  }
  mArrayToAccesses.erase(I);
}

void DIArrayHandle::deleted() {
  assert(mAccessInfo && "List of array accesses must not be null!");
  LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: delete ";
             printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
             dbgs() << "\n");
  mAccessInfo->erase(getMemoryPtr());
}

void DIArrayHandle::allUsesReplacedWith(DIMemory *M) {
  assert(M != getMemoryPtr() &&
         "Old and new memory locations must not be equal!");
  assert(mAccessInfo && "List of array accesses must not be null!");
  LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: replace ";
             // We use C representation to print memory location because
             // representation details are not important here.
             printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
             dbgs() << " with ";
             printDILocationSource(dwarf::DW_LANG_C, *M, dbgs());
             dbgs() << "\n");
  // Make a temporary copy, because insertion of new accesses updates DenseSet
  // which contains the current access. Hence, this access could be invalidated.
  auto *AccessInfo{mAccessInfo};
  for (auto &Access : AccessInfo->array_accesses(getMemoryPtr())) {
    auto NewAccess = std::make_unique<DIArrayAccess>(
        M, Access.getParent(), Access.size(), Access.getReadInfo(),
        Access.getWriteInfo());
    for (unsigned DimIdx = 0, DimIdxE = Access.size(); DimIdx < DimIdxE;
         ++DimIdx)
      if (Access[DimIdx])
        Access[DimIdx]->apply([&NewAccess](const auto &To) {
          NewAccess->make<std::decay_t<decltype(To)>>(To.getDimension(), To);
        });
    SmallVector<DIArrayAccess::Scope, 8> Nest;
    AccessInfo->scopes(Access, Nest);
    AccessInfo->add(NewAccess.release(), Nest);
  }
  AccessInfo->erase(getMemoryPtr());
}

void DIArrayAccessInfo::printScope(
    const ScopeToAccessMap::const_iterator &ScopeItr, unsigned Offset,
    unsigned OffsetStep, Optional<unsigned> DWLang,
    llvm::DenseMap<const DIArrayAccess *, AccessPool::size_type> &AccessMap,
    llvm::DenseMap<Scope, std::vector<ScopeToAccessMap::const_iterator>>
        &Children,
    llvm::raw_ostream &OS) const {
  OS << std::string(Offset, ' ');
  // Print IDs of boundary accesses in the current scope.
  OS << "[" << AccessMap[&*ScopeItr->get<Begin>()] << ", "
     << (ScopeItr->get<End>() != mAccesses.end()
             ? AccessMap[&*ScopeItr->get<End>()]
             : AccessMap.size())
     << ")";
  // Print the scope description.
  if (auto DISub = dyn_cast<DISubprogram>(ScopeItr->get<Scope>())) {
    OS << " subprogram '" << DISub->getName() << "' at " << DISub->getFilename()
       << ":" << DISub->getLine();
    if (!DWLang)
      if (auto CU = DISub->getUnit())
        DWLang = CU->getSourceLanguage();
  } else {
    auto *S = ScopeItr->get<Scope>();
    for (unsigned I = 0, EI = S->getNumOperands(); I < EI; ++I)
      if (auto *L = dyn_cast<DILocation>(S->getOperand(I))) {
        uint64_t ID = 0;
        bcl::shrinkPair(L->getLine(), L->getColumn(), ID);
        OS << " L" << ID << " at ";
        tsar::print(OS, L);
        break;
      }
  }
  OS << "\n";
  Offset += OffsetStep;
  // Print boundary accesses to each array in the scope.
  for (auto &ArrayInfo : ScopeItr->get<ArrayToAccessMap>()) {
    OS << std::string(Offset, ' ');
    if (DWLang)
      printDILocationSource(*DWLang, *ArrayInfo.get<Array>(), OS);
    else
      OS << "?";
    OS << ": [" << AccessMap[&*ArrayInfo.get<Begin>()] << ", "
       << (ArrayInfo.get<End>() != mArrayAccesses.end()
               ? AccessMap[&*ArrayInfo.get<End>()]
               : AccessMap.size())
       << ")";
    OS << "\n";
  }
  // Print description of each access in the scope.
  for (auto AccessItr = ScopeItr->get<Begin>();
       AccessItr != ScopeItr->get<End>() &&
       AccessItr->getParent() == ScopeItr->get<Scope>();
       ++AccessItr) {
    auto AccessID = AccessMap[&*AccessItr];
    OS << AccessID << ":";
    auto DigitNum = bcl::numberOfDigits(AccessID, decltype(AccessID)(10));
    if (DigitNum < Offset)
      OS << std::string(Offset - DigitNum, ' ');
    if (DWLang)
      printDILocationSource(*DWLang, *AccessItr->getArray(), OS);
    else
      OS << "?";
    if (DWLang && isFortran(*DWLang))
      OS << "(";
    for (unsigned DimIdx = 0, DimIdxE = AccessItr->size(); DimIdx < DimIdxE;
         ++DimIdx) {
      if (DWLang && isFortran(*DWLang)) {
        if (DimIdx > 0)
          OS << ", ";
      } else {
        OS << "[";
      }
      if ((*AccessItr)[DimIdx])
        (*AccessItr)[DimIdx]->print(OS);
      else
        OS << "?";
      if (!(DWLang && isFortran(*DWLang)))
        OS << "]";
    }
    if (DWLang && isFortran(*DWLang))
      OS << ")";
    // Print link to the nest access in the list of accesses which is sorted
    // according to arrays.
    auto NextArrayItr = array_iterator(*AccessItr);
    ++NextArrayItr;
    OS << " -> "
       << (NextArrayItr != mArrayAccesses.end() ? AccessMap[&*NextArrayItr]
                                                : AccessMap.size());
    OS << "\n";
  }
  // Print inner scopes.
  auto ChildrenItr = Children.find(ScopeItr->get<Scope>());
  if (ChildrenItr == Children.end())
    return;
  sort(ChildrenItr->second, [&AccessMap](auto &LHS, auto &RHS) {
    return AccessMap[&*LHS->template get<Begin>()] <
           AccessMap[&*RHS->template get<Begin>()];
  });
  for (auto &ChildItr : ChildrenItr->second)
    printScope(ChildItr, Offset, OffsetStep, DWLang, AccessMap, Children, OS);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD
void DIArrayAccessInfo::dump() const { print(dbgs()); }
#endif

void DIArrayAccessInfo::print(raw_ostream &OS) const {
  DenseMap<const DIArrayAccess *, AccessPool::size_type> AccessMap;
  DenseMap<Scope, std::vector<ScopeToAccessMap::const_iterator>> Children;
  std::vector<ScopeToAccessMap::const_iterator> OutermostScopes;
  AccessPool::size_type AccessID = 0;
  for (auto &Access : mAccesses)
    AccessMap.try_emplace(&Access, AccessID++);
  for (auto I = mScopeToAccesses.begin(), EI = mScopeToAccesses.end(); I != EI;
    ++I)
    if (auto P = I->get<Parent>())
      Children.try_emplace(P).first->second.push_back(I);
    else
      OutermostScopes.push_back(I);
  sort(OutermostScopes, [&AccessMap](auto &LHS, auto &RHS) {
    return AccessMap[&*LHS->template get<Begin>()] <
           AccessMap[&*RHS->template get<Begin>()];
  });
  unsigned OffsetStep = 2;
  unsigned Offset = 5; // Indentation for access ID.
  OS << "Scope accesses:\n";
  for (auto &ScopeItr : OutermostScopes)
    printScope(ScopeItr, Offset, OffsetStep, Optional<unsigned>{}, AccessMap,
               Children, OS);
  OS << "Array accesses:";
  for (auto &ArrayInfo : mArrayToAccesses) {
    OS << " [" << AccessMap[&*ArrayInfo.get<Begin>()] << ", "
       << (ArrayInfo.get<End>() != mArrayAccesses.end()
               ? AccessMap[&*ArrayInfo.get<End>()]
               : AccessMap.size())
       << ")";
  }
  OS << "\n";
}

namespace {
class DIArrayAccessStorage : public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  DIArrayAccessStorage() : ImmutablePass(ID) {
    initializeDIArrayAccessStoragePass(*PassRegistry::getPassRegistry());
  }

  DIArrayAccessInfo &getAccessInfo() noexcept { return mAccessInfo; }
  const DIArrayAccessInfo &getAccessInfo() const noexcept {
    return mAccessInfo;
  }

private:
  DIArrayAccessInfo mAccessInfo;
};

class DIArrayAccessCollector : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  DIArrayAccessCollector() : ModulePass(ID) {
    initializeDIArrayAccessCollectorPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

using DIArrayAccessCollectorProvider =
    FunctionPassProvider<DominatorTreeWrapperPass, DelinearizationPass,
                         LoopInfoWrapperPass, ScalarEvolutionWrapperPass,
                         EstimateMemoryPass, DIEstimateMemoryPass>;

/// Convert representation of array access in LLVM IR to apc::ArrayInfo.
struct IRToArrayInfoFunctor {
  void operator()(Instruction &I, MemoryLocation &&Loc, unsigned OpIdx,
                  AccessInfo IsRead, AccessInfo IsWrite);

  const GlobalOptions &GlobalOpts;
  DIArrayAccessCollectorProvider &Provider;
  DIMemory &ArrayDIM;
  ObjectID Subroutine;
  Array::Range &Range;
  DIArrayAccessInfo &Accesses;
};
} // namespace

char DIArrayAccessWrapper::ID = 0;
INITIALIZE_PASS_IN_GROUP(DIArrayAccessWrapper, "di-array-access",
                         "Array Accesses Collector (Metadata, Wrapper)", true,
                         true,
                         DefaultQueryManager::PrintPassGroup::getPassRegistry())

char DIArrayAccessStorage::ID = 0;
INITIALIZE_PASS(DIArrayAccessStorage, "di-array-access-is",
                "Array Accesses Collector (Metadata, Storage)", true, true)

INITIALIZE_PROVIDER_BEGIN(DIArrayAccessCollectorProvider,
                          "di-array-access-provider",
                          "Array Accesses Collector (Metadata, Provider)")
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PROVIDER_END(DIArrayAccessCollectorProvider,
                        "di-array-access-provider",
                        "Array Accesses Collector (Metadata, Provider)")

char DIArrayAccessCollector::ID = 0;
INITIALIZE_PASS_BEGIN(DIArrayAccessCollector, "di-array-access-collector",
                      "Array Accesses Collector (Metadata)", true, true)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DIArrayAccessStorage)
INITIALIZE_PASS_DEPENDENCY(DIArrayAccessCollectorProvider)
INITIALIZE_PASS_END(DIArrayAccessCollector, "di-array-access-collector",
                    "Array Accesses Collector (Metadata)", true, true)

ModulePass *llvm::createDIArrayAccessCollector() {
  return new DIArrayAccessCollector;
}

ImmutablePass *llvm::createDIArrayAccessStorage() {
  return new DIArrayAccessStorage;
}

void DIArrayAccessCollector::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<DIArrayAccessCollectorProvider>();
  AU.setPreservesAll();
}

bool DIArrayAccessCollector::runOnModule(Module &M) {
  auto *Storage = getAnalysisIfAvailable<DIArrayAccessStorage>();
  if (!Storage)
    return false;
  auto &Accesses = Storage->getAccessInfo();
  auto &DL = M.getDataLayout();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &DIMEnv = getAnalysis<DIMemoryEnvironmentWrapper>();
  DIArrayAccessCollectorProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GlobalOpts](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GlobalOpts);
      });
  DIArrayAccessCollectorProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) { Wrapper.set(*DIMEnv); });
  if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
    DIArrayAccessCollectorProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  for (auto &F : M) {
    if (F.empty())
      continue;
    auto *DISub = findMetadata(&F);
    if (!DISub)
      continue;
    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    auto &Provider = getAnalysis<DIArrayAccessCollectorProvider>(F);
    auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto &DI = Provider.get<DelinearizationPass>().getDelinearizeInfo();
    auto &AT = Provider.get<EstimateMemoryPass>().getAliasTree();
    auto &DIAT = Provider.get<DIEstimateMemoryPass>().getAliasTree();
    for (auto *A : DI.getArrays()) {
      if (!A->isDelinearized() || !A->hasMetadata())
        continue;
      auto *EM = AT.find(MemoryLocation(A->getBase(), 0));
      assert(EM && "Estimate memory must be presented in alias tree!");
      auto RawDIM = getRawDIMemoryIfExists(*EM->getTopLevelParent(),
                                           F.getContext(), DL, DT);
      if (!RawDIM)
        continue;
      auto DIMItr = DIAT.find(*RawDIM);
      assert(DIMItr != DIAT.memory_end() &&
             "Existing memory must be presented in metadata alias tree.");
      for (auto &Range : *A) {
        if (!Range.isValid())
          continue;
        for (auto *U : Range.Ptr->users()) {
          if (!isa<Instruction>(U))
            continue;
          for_each_memory(
              cast<Instruction>(*U), TLI,
              IRToArrayInfoFunctor{GlobalOpts, Provider, *DIMItr, DISub, Range,
                                   Accesses},
              [](Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {});
        }
      }
    }
  }
  LLVM_DEBUG(Accesses.print(dbgs()));
  return false;
}

void IRToArrayInfoFunctor::operator()(Instruction &I, MemoryLocation &&Loc,
                                      unsigned OpIdx, AccessInfo IsRead,
                                      AccessInfo IsWrite) {
  if (Loc.Ptr != Range.Ptr)
    return;
  auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  auto InnerLoop = LI.getLoopFor(I.getParent());
  if (!InnerLoop)
    return;
  auto InnerLoopID = InnerLoop->getLoopID();
  if (!InnerLoopID)
    return;
  SmallVector<ObjectID, 5> LoopNest;
  LoopNest.push_back(InnerLoopID);
  auto CurrLoop = InnerLoop;
  while (CurrLoop = CurrLoop->getParentLoop())
    if (auto *ID = CurrLoop->getLoopID())
      LoopNest.push_back(ID);
    else
      return;
  LoopNest.push_back(Subroutine);
  auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
  auto Access = std::make_unique<DIArrayAccess>(
      &ArrayDIM, InnerLoopID, Range.Subscripts.size(), IsRead, IsWrite);
  LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: access to '";
             if (auto DWLang = getLanguage(*I.getFunction()))
                 printDILocationSource(*DWLang, ArrayDIM, dbgs());
             dbgs() << " at "; I.getDebugLoc().print(dbgs());
             dbgs() << ", access type ";
             if (IsWrite != AccessInfo::No) dbgs() << "write";
             if (IsRead != AccessInfo::No) dbgs() << "read"; dbgs() << "\n");
  for (std::size_t DimIdx = 0, DimIdxE = Range.Subscripts.size();
       DimIdx < DimIdxE; ++DimIdx) {
    auto *S = Range.Subscripts[DimIdx];
    auto AddRecInfo = computeSCEVAddRec(S, SE);
    const SCEV *Coef, *ConstTerm;
    const Loop *L = nullptr;
    if ((!GlobalOpts.IsSafeTypeCast || AddRecInfo.second) &&
        isa<SCEVAddRecExpr>(AddRecInfo.first)) {
      auto AddRec = cast<SCEVAddRecExpr>(AddRecInfo.first);
      Coef = AddRec->getStepRecurrence(SE);
      ConstTerm = AddRec->getStart();
      L = AddRec->getLoop();
    } else {
      Coef = SE.getZero(S->getType());
      ConstTerm = S;
    }
    SmallVector<std::tuple<DIMemory *, const SCEV *, const SCEV *>, 2> Terms;
    DIMemory *VariableTerm{nullptr};
    if (!isa<SCEVConstant>(ConstTerm) || !isa<SCEVConstant>(Coef)) {
      if (!L || !L->getLoopPreheader())
        continue;
      using TypeQueqT = SmallVector<PointerIntPair<Type *, 1, bool>, 1>;
      std::pair<TypeQueqT, TypeQueqT> TypeQueue;
      auto stripTypeCast = [](const SCEV *S, auto &TQ) {
        while (auto Cast{ dyn_cast<SCEVCastExpr>(S) }) {
          if (isa<SCEVSignExtendExpr>(Cast))
            TQ.emplace_back(Cast->getType(), true);
          else
            TQ.emplace_back(Cast->getType(), false);
          S = Cast->getOperand();
        }
        return S;
      };
      ConstTerm = stripTypeCast(ConstTerm, TypeQueue.first);
      Coef = stripTypeCast(Coef, TypeQueue.second);
      for (auto I{L->getHeader()->begin()}; isa<PHINode>(I); ++I) {
        auto *Phi{cast<PHINode>(I)};
        InductionDescriptor ID;
        PredicatedScalarEvolution PSE{SE, const_cast<Loop &>(*L)};
        if (!InductionDescriptor::isInductionPHI(Phi, L, PSE, ID))
          continue;
        if (!SE.isSCEVable(ID.getStartValue()->getType()))
          continue;
        auto Start{SE.getSCEV(ID.getStartValue())};
        if (ConstTerm->getType() != Start->getType())
          continue;
        auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
        auto Incoming{find_if(Phi->incoming_values(), [L, Phi](const auto &U) {
          return L->contains(Phi->getIncomingBlock(U));
        })};
        Instruction *Users[] = {&Phi->getIncomingBlock(*Incoming)->back()};
        SmallVector<DIMemoryLocation, 2> DILocs;
        auto DILoc{findMetadata(*Incoming, Users, DT, DILocs)};
        if (!DILoc)
          continue;
        auto &DIAT{Provider.get<DIEstimateMemoryPass>().getAliasTree()};
        auto RawDIM{getRawDIMemoryIfExists(Phi->getContext(), *DILoc)};
        if (!RawDIM)
          continue;
        auto DIMItr{DIAT.find(*RawDIM)};
        assert(DIMItr != DIAT.memory_end() &&
               "Existing memory must be presented in metadata alias "
               "tree.");
        auto ConstT{SE.getMinusSCEV(ConstTerm, Start)};
        auto Div{divide(SE, Coef, ID.getStep(), GlobalOpts.IsSafeTypeCast)};
        auto CoefT{Div.Remainder->isZero() ? Div.Quotient
                                           : SE.getCouldNotCompute()};
        auto addTypeCast = [&SE](const SCEV * S, auto &TQ) {
          for (auto &T : reverse(TQ))
            S = T.getInt() ? SE.getTruncateOrSignExtend(S, T.getPointer())
                           : SE.getTruncateOrZeroExtend(S, T.getPointer());
          return S;
        };
        ConstT = addTypeCast(ConstT, TypeQueue.first);
        CoefT = addTypeCast(CoefT, TypeQueue.second);
        if (isa<SCEVConstant>(ConstT) && isa<SCEVConstant>(CoefT))
          Terms.emplace_back(&*DIMItr, ConstT, CoefT);
      }
      if (Terms.empty())
        continue;
      VariableTerm = std::get<0>(Terms.back());
      ConstTerm = std::get<1>(Terms.back());
      Coef = std::get<2>(Terms.back());
    }
    ObjectID MonomLoopID = nullptr;
    if (L) {
      MonomLoopID = L->getLoopID();
      if (!MonomLoopID)
        continue;
    }
    auto ConstTermValue = cast<SCEVConstant>(ConstTerm)->getAPInt();
    auto SymbolKind{VariableTerm ? DIAffineSubscript::Symbol::SK_Induction
                                 : DIAffineSubscript::Symbol::SK_Constant};
    DIAffineSubscript::Symbol Symbol{SymbolKind, APSInt(ConstTermValue, false),
                                     VariableTerm};
    auto *DimAccess{Access->make<DIAffineSubscript>(DimIdx, Symbol)};
    if (L) {
      auto CoefValue = cast<SCEVConstant>(Coef)->getAPInt();
      DIAffineSubscript::Symbol CoefSymbol{SymbolKind, APSInt(CoefValue, false),
                                           VariableTerm};
      DimAccess->emplaceMonom(MonomLoopID, CoefSymbol);
    }
    LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: dimension " << DimIdx
                      << " subscript ";
               DimAccess->print(dbgs()); dbgs() << "\n");
  }
  Accesses.add(Access.release(), LoopNest);
}

void DIArrayAccessWrapper::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

static void
copySubscriptToClient(const DIAffineSubscript &Subscript,
                      DenseMap<Metadata *, const Loop *> &ServerToClientLoop,
                      ClonedDIMemoryMatcher &MemoryMatcher,
                      DIArrayAccess &Access) {
  auto copySymbol = [&MemoryMatcher](const DIAffineSubscript::Symbol &S) {
    auto Symbol{S};
    if (S.Variable) {
      if (auto I{MemoryMatcher.find<Clone>(&*S.Variable)};
          I != MemoryMatcher.end())
        Symbol.Variable = I->get<Origin>();
      else
        Symbol.Variable = nullptr;
    }
    return Symbol;
  };
  auto Symbol{copySymbol(Subscript.getSymbol())};
  auto *ClientAccess =
      Access.make<DIAffineSubscript>(Subscript.getDimension(), Symbol);
  for (auto I : seq(0u, Subscript.getNumberOfMonoms())) {
    auto LoopItr = ServerToClientLoop.find(Subscript.getMonom(I).Column);
    if (LoopItr == ServerToClientLoop.end()) {
      Access.reset(Subscript.getDimension());
      break;
    }
    auto Symbol{copySymbol(Subscript.getMonom(I).Value)};
    ClientAccess->emplaceMonom(LoopItr->second->getLoopID(), std::move(Symbol));
  }
  LLVM_DEBUG(
      dbgs()
      << "[DI ARRAY ACCESS]: successfully copy affine subscript at dimension "
      << Subscript.getDimension() << "\n");
}

bool DIArrayAccessWrapper::runOnModule(Module &M) {
  releaseMemory();
  auto *Storage = getAnalysisIfAvailable<DIArrayAccessStorage>();
  if (Storage) {
    mIsInfoOwner = false;
    mAccessInfo = &Storage->getAccessInfo();
    return false;
  }
  auto *SInfo = getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>();
  if (!SInfo)
    return false;
  auto ActiveItr = (*SInfo)->getActive();
  if (ActiveItr == (*SInfo)->end())
    return false;
  auto &Socket = ActiveItr->second;
  auto R =
      Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
                         ClonedDIMemoryMatcherWrapper, DIArrayAccessWrapper>();
  if (!R)
    return false;
  LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: load accesses from server\n");
  auto &Matcher = *R->value<AnalysisClientServerMatcherWrapper *>();
  auto &MemoryMatcher = R->value<ClonedDIMemoryMatcherWrapper *>()->get();
  auto ServerAccesses = R->value<DIArrayAccessWrapper *>()->getAccessInfo();
  if (!ServerAccesses)
    return false;
  LLVM_DEBUG(ServerAccesses->print(dbgs()));
  mIsInfoOwner = true;
  mAccessInfo = new DIArrayAccessInfo;
  for (auto &F : M) {
    if (F.empty())
      continue;
    auto ServerFItr = Matcher->find(&F);
    if (ServerFItr == Matcher->end() || !(ServerFItr->second))
      continue;
    auto DIATMemory = MemoryMatcher.find(cast<Function>(*ServerFItr->second));
    if (!DIATMemory)
      continue;
    auto FuncMD = F.getSubprogram();
    if (!FuncMD)
      continue;
    auto ServerFuncMD = findMetadata(cast<Function>(ServerFItr->second));
    if (!ServerFuncMD)
      continue;
    DenseMap<Metadata *, const Loop *> ServerToClientLoop;
    auto &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    for_each_loop(LI, [&Matcher, &ServerToClientLoop](const Loop *L) {
      if (auto LoopID = L->getLoopID())
        if (auto ServerLoopID = Matcher->getMappedMD(LoopID))
          ServerToClientLoop.try_emplace(*ServerLoopID, L);
    });
    LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: process function " << F.getName()
                      << "\n");
    for (auto &Access : ServerAccesses->scope_accesses(ServerFuncMD)) {
      LLVM_DEBUG(
          dbgs() << "[DI ARRAY ACCESS]: copy access from server ";
          if (auto DWLang = getLanguage(cast<Function>(*ServerFItr->second)))
              printDILocationSource(*DWLang, *Access.getArray(), dbgs());
          dbgs() << "\n");
      auto ArrayItr = DIATMemory->find<Clone>(Access.getArray());
      if (ArrayItr == DIATMemory->end())
        continue;
      LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: memory found on client\n");
      SmallVector<ObjectID, 5> LoopNest;
      if (ServerFuncMD != Access.getParent()) {
        auto ParentItr = ServerToClientLoop.find(Access.getParent());
        if (ParentItr == ServerToClientLoop.end())
          continue;
        auto CurrLoop = ParentItr->second;
        LoopNest.push_back(CurrLoop->getLoopID());
        while (CurrLoop = CurrLoop->getParentLoop())
          if (auto *ID = CurrLoop->getLoopID()) {
            LoopNest.push_back(ID);
          } else {
            LoopNest.clear();
            break;
          }
        if (LoopNest.empty())
          continue;
      }
      LLVM_DEBUG(dbgs() << "[DI ARRAY ACCESS]: build scope nest on client\n");
      LoopNest.push_back(FuncMD);
      auto A = std::make_unique<DIArrayAccess>(
          ArrayItr->get<Origin>(), LoopNest.front(), Access.size(),
          Access.getReadInfo(), Access.getWriteInfo());
      for (auto DimIdx : seq(0u, Access.size())) {
        if (Access[DimIdx])
          Access[DimIdx]->apply(
              [&A, &ServerToClientLoop, DIATMemory](const auto &To) {
                copySubscriptToClient(To, ServerToClientLoop, *DIATMemory, *A);
              });
      }
      mAccessInfo->add(A.release(), LoopNest);
    }
  }
  LLVM_DEBUG(mAccessInfo->print(dbgs()));
  return false;
}
