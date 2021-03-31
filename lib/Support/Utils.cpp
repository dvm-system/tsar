//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
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

#include "tsar/Support/Utils.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include <llvm/IR/IntrinsicInst.h>
#include <regex>

using namespace llvm;
using namespace tsar;

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

llvm::Optional<unsigned> getLanguage(const llvm::DIVariable &DIVar) {
  auto Scope = DIVar.getScope();
  while (Scope) {
    auto *CU = isa<DISubprogram>(Scope) ?
      cast<DISubprogram>(Scope)->getUnit() : dyn_cast<DICompileUnit>(Scope);
    if (CU)
      return CU->getSourceLanguage();
    Scope = Scope->getScope();
  }
  return None;
}

llvm::Optional<uint64_t> getConstantCount(
    const llvm::DISubrange &Range) {
  auto DICount = Range.getCount();
  if (!DICount)
    return None;
  if (!DICount.is<ConstantInt*>())
    return None;
  auto Count = DICount.get<ConstantInt *>()->getValue();
  if (!Count.isStrictlyPositive())
    return None;;
  return Count.getSExtValue();
}

llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo) {
  auto ArgItr = F.arg_begin();
  auto ArgItrE = F.arg_end();
  for (std::size_t I = 0; ArgItr != ArgItrE && I <= ArgNo; ++I, ++ArgItr);
  return ArgItr != ArgItrE ? &*ArgItr : nullptr;
}

bool pointsToLocalMemory(const Value &V, const Loop &L) {
  if (!isa<AllocaInst>(V))
    return false;
  bool StartInLoop{false}, EndInLoop{false};
  for (auto *V1 : V.users())
    if (auto *BC{dyn_cast<BitCastInst>(V1)})
      for (auto *V2 : BC->users())
        if (auto *II{dyn_cast<IntrinsicInst>(V2)}) {
          auto *BB{II->getParent()};
          if (L.contains(BB)) {
            auto ID{II->getIntrinsicID()};
            if (!StartInLoop && ID == llvm::Intrinsic::lifetime_start)
              StartInLoop = true;
            else if (!EndInLoop && ID == llvm::Intrinsic::lifetime_end)
              EndInLoop = true;
            if (StartInLoop && EndInLoop)
              return true;
          }
        }
  return false;
}
}
