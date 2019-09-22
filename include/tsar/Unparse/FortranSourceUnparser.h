//===- FortranSourceUnparser.h - Fortran Source Info Unparser ---*- C++ -*-===//
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
// This file defines unparser to print metadata objects as constructs in Fortran.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FORTRAN_SOURCE_UNPARSER_H
#define TSAR_FORTRAN_SOURCE_UNPARSER_H

#include "tsar/Unparse/SourceUnparser.h"
#include <llvm/ADT/APInt.h>

namespace tsar {
/// This is an unparser for Fortran language.
class FortranSourceUnparser : public SourceUnparser<FortranSourceUnparser> {
public:
  ///Creates unparser for a specified expression.
  explicit FortranSourceUnparser(const DIMemoryLocation &Loc,
      bool Minimal) noexcept :
    SourceUnparser(Loc, true, Minimal) {}

private:
  friend SourceUnparser<FortranSourceUnparser>;

  void appendToken(Token T, bool IsSubscript,
      llvm::SmallVectorImpl<char> &Str) {
    switch (T) {
    default: llvm_unreachable("Unsupported kind of token!"); break;
    case TOKEN_ADDRESS: Str.push_back('&'); break;
    case TOKEN_DEREF: Str.push_back('*'); break;
    case TOKEN_PARENTHESES_LEFT: Str.push_back('('); break;
    case TOKEN_PARENTHESES_RIGHT: Str.push_back(')'); break;
    case TOKEN_ADD: Str.push_back('+'); break;
    case TOKEN_SUB: Str.push_back('-'); break;
    case TOKEN_FIELD: Str.push_back('%'); break;
    case TOKEN_UNKNOWN: Str.push_back('?'); break;
    case TOKEN_SEPARATOR:
      if (IsSubscript)
        Str.push_back(',');
      break;
    case TOKEN_CAST_TO_ADDRESS:
      Str.append({ '(', '?', 'b', 'y', 't', 'e', '*', '?', ')' }); break;
    }
  }

    void appendUConst(
      uint64_t C, bool IsSubscript, llvm::SmallVectorImpl<char> &Str) {
    llvm::APInt Const(64, C, false);
    Const.toString(Str, 10, false);
  }


  void beginSubscript(llvm::SmallVectorImpl<char> &Str) {
    Str.push_back('(');
  }

  void endSubscript(llvm::SmallVectorImpl<char> &Str) {
    Str.push_back(')');
  }
};
}
#endif//TSAR_FORTRAN_SOURCE_UNPARSER_H

