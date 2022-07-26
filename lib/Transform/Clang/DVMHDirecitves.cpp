//===- DVMHDirectives.cpp - Representation of DVMH Directives ----*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file provides class to represent DVMH directives.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

using namespace llvm;
using namespace tsar;
using namespace tsar::dvmh;

PragmaParallel *tsar::dvmh::isParallel(const Loop *L,
                                       Parallelization &ParallelizationInfo) {
  if (auto ID = L->getLoopID()) {
    auto Ref{ParallelizationInfo.find<PragmaParallel>(L->getHeader(), ID)};
    return cast_or_null<PragmaParallel>(Ref);
  }
  return nullptr;
}

std::pair<PragmaParallel *, Loop *>
tsar::dvmh::isInParallelNest(const Instruction &I, const LoopInfo &LI,
                             Parallelization &PI) {
  auto *L{LI.getLoopFor(I.getParent())};
  while (L) {
    if (auto *P{isParallel(L, PI)})
      return std::pair{P, L};
    L = L->getParentLoop();
  }
  return std::pair{nullptr, nullptr};
}

unsigned tsar::dvmh::processRegularDependencies(
    ObjectID LoopID, const SCEVConstant *ConstStep,
    const ClangDependenceAnalyzer::ASTRegionTraitInfo &ASTDepInfo,
    DIArrayAccessInfo &AccessInfo, ShadowVarListT &AcrossInfo) {
  assert(!ASTDepInfo.get<trait::Dependence>().empty() &&
         "Unable to process loop without data dependencies!");
  unsigned PossibleAcrossDepth =
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().empty() ?
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Anti>().size() :
    ASTDepInfo.get<trait::Dependence>()
        .begin()->second.get<trait::Flow>().size();
  auto updatePAD = [&PossibleAcrossDepth](auto &Dep, auto T) {
    if (!Dep.second.template get<decltype(T)>().empty()) {
      auto MinDepth = *Dep.second.template get<decltype(T)>().front().first;
      unsigned PAD = 1;
      for (auto InnerItr = Dep.second.template get<decltype(T)>().begin() + 1,
                InnerItrE = Dep.second.template get<decltype(T)>().end();
           InnerItr != InnerItrE; ++InnerItr, ++PAD)
        if (InnerItr->first->isNegative()) {
          auto Revert = -(*InnerItr->first);
          Revert.setIsUnsigned(true);
          if (Revert >= MinDepth)
            break;
        }
      PossibleAcrossDepth = std::min(PossibleAcrossDepth, PAD);
    }
  };
  for (auto &Dep : ASTDepInfo.get<trait::Dependence>()) {
    auto AccessItr =
        find_if(AccessInfo.scope_accesses(LoopID), [&Dep](auto &Access) {
            return Access.getArray() == Dep.first.get<MD>();
        });
    if (AccessItr == AccessInfo.scope_end(LoopID))
      return 0;
    Optional<unsigned> DependentDim;
    unsigned NumberOfDims = 0;
    for (auto &Access :
         AccessInfo.array_accesses(AccessItr->getArray(), LoopID)) {
      NumberOfDims = std::max(NumberOfDims, Access.size());
      for (auto *Subscript : Access) {
        if (!Subscript || !isa<DIAffineSubscript>(Subscript))
          return 0;
        auto Affine = cast<DIAffineSubscript>(Subscript);
        ObjectID AnotherColumn = nullptr;
        for (unsigned Idx = 0, IdxE = Affine->getNumberOfMonoms(); Idx < IdxE;
             ++Idx) {
          if (Affine->getMonom(Idx).Column == LoopID) {
            if (AnotherColumn ||
                DependentDim && *DependentDim != Affine->getDimension())
              return 0;
            DependentDim = Affine->getDimension();
          } else {
            if (DependentDim && *DependentDim == Affine->getDimension())
              return 0;
            AnotherColumn = Affine->getMonom(Idx).Column;
          }
        }
      }
    }
    if (!DependentDim)
      return 0;
    updatePAD(Dep, trait::Flow{});
    updatePAD(Dep, trait::Anti{});
    auto I{AcrossInfo.try_emplace(Dep.first).first};
    I->second.get<trait::DIDependence::DistanceVector>().resize(NumberOfDims);
    I->second.get<Corner>() = false;
    auto getDistance = [&Dep](auto T) {
      return Dep.second.get<decltype(T)>().empty()
                 ? None
                 : Dep.second.get<decltype(T)>().front().second;
    };
    if (ConstStep->getAPInt().isNegative())
      I->second.get<trait::DIDependence::DistanceVector>()[*DependentDim] = {
          getDistance(trait::Anti{}), getDistance(trait::Flow{})};

    else
      I->second.get<trait::DIDependence::DistanceVector>()[*DependentDim] = {
          getDistance(trait::Flow{}), getDistance(trait::Anti{})};
  }
  return PossibleAcrossDepth;
}
