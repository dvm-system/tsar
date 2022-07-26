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
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Support/raw_os_ostream.h>

using namespace llvm;
using namespace tsar;

namespace tsar {
void printAPCVersion(llvm::raw_ostream &OS) {
  OS << "APC (" << APC_HOMEPAGE_URL << "):\n";
  OS << "    version " << APC_VERSION_STRING << "\n";
  OS << "    language ";
#ifdef APC_C
  OS << "C/C++";
#endif
  OS << "\n";
}

SmallVector<std::size_t, 8>
extractTplDimsAlignmentIndexes(const apc::AlignRule &AR) {
  SmallVector<std::size_t, 8> TplDimAR(AR.alignWith->GetDimSize(),
                                       AR.alignWith->GetDimSize());
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

void messagePrint(const Twine &File, const apc::Messages &Msg,
                  raw_ostream &OS) {
  std::pair<unsigned, unsigned> Loc;
  bcl::restoreShrinkedPair(Msg.line, Loc.first, Loc.second);
  OS << File << ":" << Loc.first << ":" << Loc.second << ": ";
  switch (Msg.type) {
  case ERROR:
    OS << "error";
    break;
  case WARR:
    OS << "warning";
    break;
  case NOTE:
    OS << "remark";
  }
  OS << ": APC" << Msg.group << " ";
  SmallString<256> Str;
  messageToString(Msg, Str);
  OS << Str << "\n";
}

void messageToString(const apc::Messages &Msg, SmallVectorImpl<char> &Out) {
  for (const wchar_t &C : Msg.engMessage) {
    auto *Raw{reinterpret_cast<const char *>(&C)};
    unsigned NonZeroCount{0u};
    for (unsigned I = 0; I < sizeof(wchar_t); ++I)
      if (Raw[I] != 0) {
        assert(NonZeroCount == 0 && "Unsupported message!");
        ++NonZeroCount;
        Out.push_back(Raw[I]);
      }
  }
}
} // namespace tsar
