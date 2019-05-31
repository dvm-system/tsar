//===--- Utils.cpp ------ Utility Methods and Classes  ----------*- C++ -*-===//
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
// This file implements abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/APC/Utils.h"
#include "DistributionUtils.h"
#include <apc/apc-config.h>
#include <apc/Distribution/DvmhDirective.h>
#include <llvm/Support/raw_os_ostream.h>

using namespace llvm;
using namespace tsar;

namespace tsar {
void printAPCVersion(llvm::raw_ostream &OS) {
  OS << "  APC (" << APC_HOMEPAGE_URL << "):\n";
  OS << "    version " << APC_VERSION_STRING << "\n";
  OS << "    language ";
#ifdef APC_C
  OS << "C";
#endif
  OS << "\n";
}

SmallVector<std::size_t, 8> extractTplDimsAlignmentIndexes(
    const apc::AlignRule &AR) {
  SmallVector<std::size_t, 8> TplDimAR(
    AR.alignWith->GetDimSize(), AR.alignWith->GetDimSize());
  assert(AR.alignArray->GetDimSize() == AR.alignRuleWith.size() &&
    "Number of alignment rules must be equal number of dims in array!");
  for (std::size_t ArrayDimIdx = 0, ArrayDimIdxE = AR.alignRuleWith.size();
    ArrayDimIdx < ArrayDimIdxE; ++ArrayDimIdx)
    if (AR.alignRuleWith[ArrayDimIdx].first >= 0)
      TplDimAR[AR.alignRuleWith[ArrayDimIdx].first] = ArrayDimIdx;
  auto NewOrder = AR.alignWith->GetNewTemplateDimsOrder();
  if (!NewOrder.empty()) {
    auto TplDimARPrev(TplDimAR);
    for (std::size_t I = 0, EI = NewOrder.size(); I < EI; ++I) {
      TplDimAR[I] = TplDimARPrev[NewOrder[I]];
    }
  }
  return TplDimAR;
}
}
