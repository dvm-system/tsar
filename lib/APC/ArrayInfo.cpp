//===---- ArrayInfo.cpp ----- APC Array Collector ---------------*- C++ -*-===//
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
// This file implements per-function pass to obtain general information about
// arrays which are accessed in a function. Not all arrays are collected, if
// some information of an array have been lost then it will be ignored.
//
//===----------------------------------------------------------------------===//

#include "AstWrapperImpl.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Support/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include <apc/Distribution/Array.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <apc/Utils/types.h>
#include <bcl/utility.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>
#include <llvm/Support/Format.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-array-info"

using namespace llvm;
using namespace tsar;

namespace {
class APCArrayInfoPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  APCArrayInfoPass() : FunctionPass(ID) {
    initializeAPCArrayInfoPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;
  void releaseMemory() override { mArrays.clear(); }

private:
  std::vector<apc::Array *> mArrays;
};
}

char APCArrayInfoPass::ID = 0;

INITIALIZE_PASS_BEGIN(APCArrayInfoPass, "apc-array-info",
  "Array Collector (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(ClangDIMemoryMatcherPass)
INITIALIZE_PASS_END(APCArrayInfoPass, "apc-array-info",
  "Array Collector (APC)", true, true)

FunctionPass * llvm::createAPCArrayInfoPass() { return new APCArrayInfoPass; }

void APCArrayInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DelinearizationPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<ClangDIMemoryMatcherPass>();
  AU.setPreservesAll();
}

bool APCArrayInfoPass::runOnFunction(Function &Func) {
  releaseMemory();
  if (hasFnAttr(Func, AttrKind::LibFunc))
    return false;
  auto *F = &Func;
  auto *DI = &getAnalysis<DelinearizationPass>().getDelinearizeInfo();
  auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto *AT = &getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto *DIAT = &getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto *DIMatcher = &getAnalysis<ClangDIMemoryMatcherPass>().getMatcher();
  auto *ClientDIAT = DIAT;
  std::function<ObjectID(const DIMemory &)> getMemoryID =
      [](const DIMemory &DIM) {
        return const_cast<MDNode *>(DIM.getAsMDNode());
      };
  std::function<MDNode *(MDNode *)> getMDNode = [](MDNode *MD) { return MD; };
  if (auto *SInfo = getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()) {
    auto ActiveItr = (*SInfo)->getActive();
    if (ActiveItr != (*SInfo)->end()) {
      auto &Socket = ActiveItr->second;
      auto Matcher = Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
        ClonedDIMemoryMatcherWrapper>();
      auto Provider = Socket.getAnalysis<
        DelinearizationPass, DominatorTreeWrapperPass,
        EstimateMemoryPass, DIEstimateMemoryPass>(Func);
      if (Matcher && Provider) {
        auto *IRMatcher = Matcher->value<AnalysisClientServerMatcherWrapper *>();
        F = cast<Function>((**IRMatcher)[&Func]);
        getMDNode = [IRMatcher](MDNode *MD) {
          auto MappedMD = (*IRMatcher)->getMappedMD(MD);
          return MappedMD ? cast<MDNode>(*MappedMD) : nullptr;
        };
        auto *MemoryMatcher =
          (**Matcher->value<ClonedDIMemoryMatcherWrapper *>())[*F];
        assert(MemoryMatcher && "Cloned memory matcher must not be null!");
        getMemoryID = [MemoryMatcher](const DIMemory &DIM) {
          auto Itr = MemoryMatcher->find<Clone>(const_cast<DIMemory *>(&DIM));
          return Itr != MemoryMatcher->end() ? Itr->get<Origin>()->getAsMDNode()
                                             : nullptr;
        };
        DI = &Provider->value<DelinearizationPass *>()->getDelinearizeInfo();
        DT = &Provider->value<DominatorTreeWrapperPass *>()->getDomTree();
        AT = &Provider->value<EstimateMemoryPass *>()->getAliasTree();
        DIAT = &Provider->value<DIEstimateMemoryPass *>()->getAliasTree();
      }
    }
  }
  if (hasFnAttr(*F, AttrKind::LibFunc))
    return false;
  auto &DL = F->getParent()->getDataLayout();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  for (auto *A: DI->getArrays()) {
    if (!A->isDelinearized() || !A->hasMetadata())
      continue;
    SmallVector<DIMemoryLocation, 4> DILocs;
    // TODO (kaniandr@gmail.com): add processing of array-members of structures.
    // Note, that delinearization of such array-member should be implemented
    // first. We can use GEP to determine which member of a structure
    // is accessed.
    auto DILoc = findMetadata(A->getBase(), DILocs, DT,
      A->isAddressOfVariable() ? MDSearch::AddressOfVariable : MDSearch::Any);
    assert(DILoc && DILoc->isValid() &&
      "Metadata must be available for an array!");
    // Ignore compile time character constants.
    if (isStubVariable(*DILoc->Var))
      continue;
    auto DIElementTy = arrayElementDIType(DILoc->Var->getType());
    if (!DIElementTy)
      continue;
    auto DeclScope = std::make_pair(Distribution::l_COMMON, std::string(""));
    if (auto DILocalVar = dyn_cast<DILocalVariable>(DILoc->Var)) {
      if (DILocalVar->isParameter())
        DeclScope.first = Distribution::l_PARAMETER;
      else
        DeclScope.first = Distribution::l_LOCAL;
      DeclScope.second =  F->getName().str();
    }
    auto UniqueName{APCCtx.getUniqueName(*DILoc->Var, *F)};
    // TODO (kaniandr@gmail.com): what should we do in case of multiple
    // allocation of the same array. There are different memory locations
    // and different MDNodes for such arrays. However, declaration points
    // for these locations are identical.
    auto *EM = AT->find(MemoryLocation(A->getBase(), 0));
    assert(EM && "Estimate memory must be presented in alias tree!");
    auto RawDIM = getRawDIMemoryIfExists(
      *EM->getTopLevelParent(), F->getContext(), DL, *DT);
    assert(RawDIM && "Unknown raw memory!");
    assert(DIAT->find(*RawDIM) != DIAT->memory_end() &&
           "Memory must exist in alias tree!");
    auto *DIEM{dyn_cast<DIEstimateMemory>(&*DIAT->find(*RawDIM))};
    // Ignore results of memory allocation, for example 'malloc' call.
    if (!DIEM)
      continue;
    // Search corresponding memory location on client if server is used.
    auto ClientRawDIM = getMemoryID(*DIEM);
    if (!ClientRawDIM)
      continue;
    assert(ClientDIAT->find(*ClientRawDIM) != ClientDIAT->memory_end() &&
           "Memory must exist in alias tree on client!");
    auto &ClientDIEM = cast<DIEstimateMemory>(*ClientDIAT->find(*ClientRawDIM));
    auto *ClientDIVar{ClientDIEM.getVariable()};
    auto DeclLoc = std::make_pair(ClientDIVar->getLine(), 0);
    SmallVector<DebugLoc, 1> DbgLocs;
    ClientDIEM.getDebugLoc(DbgLocs);
    if (!DbgLocs.empty()) {
      DebugLoc DbgLoc;
      for (auto &Loc : DbgLocs) {
        if (!Loc || Loc.getLine() != ClientDIVar->getLine() ||
            cast<DIScope>(Loc.getScope())->getFilename() !=
                ClientDIVar->getFilename())
          continue;
        if (!DbgLoc || Loc.getCol() < DbgLoc.getCol())
          DbgLoc = Loc;
      }
      DeclLoc = std::make_pair(DbgLoc->getLine(), DbgLoc->getColumn());
    }
    std::decay<
      decltype(std::declval<apc::Array>().GetDeclInfo())>
        ::type::value_type::second_type ShrinkedDeclLoc;
    auto getDbgLoc = [&F, &DbgLocs, &DeclLoc]() {
      DebugLoc DbgLoc;
      if (!DbgLocs.empty() && DbgLocs.front())
        DbgLoc = DbgLocs.front();
      else if (F->getSubprogram())
        DbgLoc = DILocation::get(F->getContext(), DeclLoc.first, DeclLoc.second,
                                 F->getSubprogram());
      return DbgLoc;
    };
    if (!bcl::shrinkPair(DeclLoc.first, DeclLoc.second, ShrinkedDeclLoc))
      emitUnableShrink(F->getContext(), *F, getDbgLoc(), DS_Warning);
    SmallString<128> PathToFile;
    StringRef Filename{F->getParent()->getSourceFileName()};
    if (ClientDIVar->getFile())
      Filename = getAbsolutePath(*ClientDIVar->getFile(), PathToFile);
    else if (sys::fs::real_path(Filename, PathToFile))
      Filename = PathToFile;
    auto DIMemoryItr = DIMatcher->find<MD>(ClientDIEM.getVariable());
    assert(DIMemoryItr != DIMatcher->end() &&
           "Unknown AST-level representation of an array!");
    tsar::dvmh::VariableT Var;
    Var.get<AST>() = DIMemoryItr->get<AST>();
    Var.get<MD>() = &ClientDIEM;
    if (auto *A{APCCtx.findArray(ClientRawDIM)}) {
      auto S{A->GetDeclSymbol()};
      S->addRedeclaration(std::move(Var));
      continue;
    }
    auto APCSymbol = new apc::Symbol(&APCCtx, std::move(Var));
    APCCtx.addSymbol(APCSymbol);
    auto APCArray = new apc::Array(
        UniqueName, Var.get<AST>()->getName().str(), A->getNumberOfDims(),
        APCCtx.getNumberOfArrays(), Filename.str(), ShrinkedDeclLoc,
        std::move(DeclScope), APCSymbol, false, false, false,
        {APCCtx.getDefaultRegion().GetName()}, getSize(DIElementTy));
    if (!APCCtx.addArray(ClientRawDIM, APCArray)) {
      llvm_unreachable("Unable to add new array to an APC context!");
      delete APCArray;
      mArrays.push_back(APCCtx.findArray(ClientRawDIM));
      continue;
    }
    mArrays.push_back(APCArray);
    auto Sizes = APCArray->GetSizes();
    for (std::size_t I = 0, EI = A->getNumberOfDims(); I < EI; ++I) {
      APCArray->SetMappedDim(I);
      Sizes[I].first = 0;
      if (auto Size = dyn_cast<SCEVConstant>(A->getDimSize(I))) {
        if (!castAPInt(Size->getAPInt(), false, Sizes[I].second))
          emitTypeOverflow(F->getContext(), *F, getDbgLoc(),
            "unable to represent upper bound of " + Twine(I+1) + "dimension",
            DS_Warning);
      }
    }
    APCArray->SetSizes(Sizes);
  }
  LLVM_DEBUG(print(dbgs(), F->getParent()));
  return false;
}

void APCArrayInfoPass::print(raw_ostream &OS, const Module *M) const {
  for (auto *A : mArrays) {
    OS << format("%s [short=%s, unique=%s, id=%d]\n", A->GetName().c_str(),
      A->GetShortName().c_str(), A->GetArrayUniqKey().c_str(), A->GetId());
    auto ScopInfo =  A->GetLocation();
    switch (ScopInfo.first) {
    case Distribution::l_LOCAL:
      OS << "  local variable in '" << ScopInfo.second << "'\n"; break;
    case Distribution::l_PARAMETER:
      OS << "  argument of '" << ScopInfo.second << "'\n"; break;
    case Distribution::l_COMMON:
      OS << "  global variable\n"; break;
    default:
      llvm_unreachable("Unsupported scope!");
    }
    auto DeclInfo = A->GetDeclInfo();
    OS << "  declaration:\n";
    for (auto &Info : DeclInfo) {
      std::pair<unsigned, unsigned> DeclLoc;
      bcl::restoreShrinkedPair(Info.second, DeclLoc.first, DeclLoc.second);
      OS << "    "
         << Info.first << ":" << DeclLoc.first << ":" << DeclLoc.second << "\n";
    }
    OS << "  size of element: " << A->GetTypeSize() << "\n";
    OS << "  number of dimensions: " << A->GetDimSize() << "\n";
    auto &DimSizes = A->GetSizes();
    for (std::size_t I = 0, EI = A->GetDimSize(); I < EI; ++I) {
      OS << "    " << I << ": size is ";
      if (DimSizes[I].second < 0)
        OS << "unknown";
      else
        OS << format("[%d, %d)", DimSizes[I].first, DimSizes[I].second);
      OS << ", may" << (A->IsDimMapped(I) ? "" : " not") << " be mapped\n";
    }
    OS << "  parallel regions: ";
    for (auto &PR : A->GetRegionsName())
      OS << PR << " ";
    OS << "\n";
  }
}
