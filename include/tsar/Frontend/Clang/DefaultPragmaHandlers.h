//===-- DefaultPrgmaHandlers.h - Pragma Handlers ----------------*- C++ -*-===//
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
// This file implements initialization of all available pragma handlers.
// Update `Add...` and `Remove...` functions to implement custom initialization
// of some handlers if necessary.
//
//===----------------------------------------------------------------------===//

#ifndef DEFAULT_PRAGMA_HANDLERS_H
#define DEFAULT_PRAGMA_HANDLERS_H

#include "tsar/Support/Directives.h"
#include "tsar/Frontend/Clang/PragmaHandlers.h"
#include <clang/Lex/Preprocessor.h>

namespace tsar {
template<class ContainerT>
void AddPragmaHandlers(clang::Preprocessor &PP, ContainerT &C) {
  for (auto NId = DirectiveNamespaceId::NotNamespace + 1;
       NId < DirectiveNamespaceId::NumNamespaces; ++NId) {
    auto NR = new PragmaNamespaceReplacer(NId);
    C.emplace_back(NR);
    PP.AddPragmaHandler(NR);
    for (auto DId = DirectiveId::NotDirective + 1;
         DId < DirectiveId::NumDirectives; ++DId) {
      if (tsar::getParent(DId) != NId)
        continue;
      if (tsar::getParent(DId) == DirectiveNamespaceId::Dvm)
        continue;
      auto *PR = new PragmaReplacer(DId, *NR);
      NR->AddPragma(PR);
      for (ClauseId CId = ClauseId::NotClause + 1;
           CId < ClauseId::NumClauses; ++CId)
        if (tsar::getParent(CId) == DId)
          PR->AddPragma(new ClauseReplacer(CId, *PR));
    }
  }
}

template<class ItrT>
void RemovePragmaHandlers(clang::Preprocessor &PP, ItrT I, ItrT E) {
  for (; I != E; ++I)
    PP.RemovePragmaHandler(*I);
}
}
#endif//DEFAULT_PRAGMA_HANDLERS_H
