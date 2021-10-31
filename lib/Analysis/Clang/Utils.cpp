
//===----- Utils.cpp------- Utility Methods and Classes ---------*- C++ -*-===//
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Clang/Utils.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Support/Tags.h"
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <clang/AST/Decl.h>

using namespace llvm;
using namespace tsar;

Optional<bool> tsar::isGrowingInduction(Loop &L, const CanonicalLoopSet &CL,
    DFRegionInfo &DFI) {
  auto CanonicalItr = CL.find_as(DFI.getRegionFor(&L));
  if (CanonicalItr == CL.end() || !(**CanonicalItr).isCanonical())
    return None;
  auto ConstStep =
      dyn_cast_or_null<SCEVConstant>((*CanonicalItr)->getStep());
  if (!ConstStep)
    return None;
  return !ConstStep->getAPInt().isNegative();
}

BitVector tsar::isGrowingInduction(Loop &Outermost, unsigned SizeOfNest,
    const CanonicalLoopSet &CL, DFRegionInfo &DFI, bool *DirectionIfUnknown) {
  BitVector IsGrowingInduction(SizeOfNest,
                               DirectionIfUnknown ? *DirectionIfUnknown : true);
  unsigned I = 0;
  auto Dir = isGrowingInduction(Outermost, CL, DFI);
  if (!Dir) {
    if (!DirectionIfUnknown)
      IsGrowingInduction.clear();
    return IsGrowingInduction;
  }
  IsGrowingInduction[I] = *Dir;
  auto *Parent = &Outermost;
  unsigned LoopDepth = 1;
  for (; LoopDepth < SizeOfNest && Parent->getSubLoops().size() == 1;
       ++LoopDepth, Parent = *Parent->begin()) {
    auto Dir = isGrowingInduction(**Parent->begin(), CL, DFI);
    if (!Dir)
      break;
    IsGrowingInduction[I] = *Dir;
  }
  if (!DirectionIfUnknown)
    IsGrowingInduction.resize(LoopDepth);
  return IsGrowingInduction;
}

bool tsar::getBaseInductionsForNest(Loop &Outermost, unsigned SizeOfNest,
    const CanonicalLoopSet &CL, const DFRegionInfo &RI,
    const MemoryMatchInfo &MM,
    SmallVectorImpl<std::pair<ObjectID, StringRef>> &Inductions) {
  auto addToInductions = [&Inductions, &CL, &MM, &RI](Loop &L) {
    auto *DFL = RI.getRegionFor(&L);
    assert(DFL && "Data-flow region must exist for a specified loop!");
    auto CanonicalItr = CL.find_as(DFL);
    if (CanonicalItr == CL.end() || !(*CanonicalItr)->isCanonical())
      return false;
    auto *Induction = (**CanonicalItr).getInduction();
    assert(Induction &&
           "Induction variable must not be null in canonical loop!");
    auto MatchItr = MM.Matcher.find<IR>(Induction);
    assert(MatchItr != MM.Matcher.end() && !MatchItr->get<AST>().empty() &&
           "AST-level variable representation must be available!");
    Inductions.emplace_back(L.getLoopID(),
                            MatchItr->get<AST>().front()->getName());
    return true;
  };
  if (!addToInductions(Outermost))
    return false;
  auto *Parent = &Outermost;
  unsigned LoopDepth = 1;
  for (;LoopDepth < SizeOfNest && Parent->getSubLoops().size() == 1;
       ++LoopDepth, Parent = *Parent->begin())
    if (!addToInductions(**Parent->begin()))
      return false;
  return LoopDepth == SizeOfNest;
}
