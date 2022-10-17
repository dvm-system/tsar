//===---- MemoryTraitJSON.h --- Memory Traits In JSON  -----------*- C++ -*===//
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
// This file defines representation of analysis results in JSON format.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/MemoryTrait.h"
#include <llvm/ADT/StringSwitch.h>

#ifndef TSAR_MEMORY_TRAIT_JSON_H
#define TSAR_MEMORY_TRAIT_JSON_H

namespace json {
/// Specialization of JSON serialization traits for
/// tsar::trait::Reduction::Kind type.
template<> struct Traits<tsar::trait::Reduction::Kind> {
  using ReductionKind = tsar::trait::Reduction::Kind;
  static bool parse(ReductionKind &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<ReductionKind>(S)
                    .CaseLower("Add", tsar::trait::Reduction::RK_Add)
                    .CaseLower("Mult", tsar::trait::Reduction::RK_Mult)
                    .CaseLower("Or", tsar::trait::Reduction::RK_Or)
                    .CaseLower("And", tsar::trait::Reduction::RK_And)
                    .CaseLower("Xor", tsar::trait::Reduction::RK_Xor)
                    .CaseLower("Max", tsar::trait::Reduction::RK_Max)
                    .CaseLower("Min", tsar::trait::Reduction::RK_Min)
                    .Default(tsar::trait::Reduction::RK_NoReduction);
      if (Dest == tsar::trait::Reduction::RK_NoReduction &&
          !Lex.isKeyword(json::Keyword::NO_VALUE))
        return false;
    } catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::trait::Reduction::Kind Obj) {
    JSON += '"';
    switch (Obj) {
      case tsar::trait::Reduction::RK_Add: JSON += "Add"; break;
      case tsar::trait::Reduction::RK_Mult: JSON += "Mult"; break;
      case tsar::trait::Reduction::RK_Or: JSON += "Or"; break;
      case tsar::trait::Reduction::RK_And: JSON += "And"; break;
      case tsar::trait::Reduction::RK_Xor: JSON += "Xor"; break;
      case tsar::trait::Reduction::RK_Max: JSON += "Max"; break;
      case tsar::trait::Reduction::RK_Min: JSON += "Min"; break;
      default:
        JSON.pop_back();
        JSON += json::toString(json::Keyword::NO_VALUE);
        return;
    }
    JSON += '"';
  }
};
}
#endif//TSAR_MEMORY_TRAIT_JSON_H
