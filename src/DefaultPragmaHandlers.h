//===-- DefaultPrgmaHandlers.h - Pragma Handlers ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

#include "Directives.h"
#include "tsar_pragma.h"
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
