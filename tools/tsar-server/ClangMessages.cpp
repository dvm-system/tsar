//===-- ClangMessages.cpp ----- JSON Messages (Clang) -----------*- C++ -*-===//
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
// This implements Clang-based messages for server-client interaction,
// where server is TSAR.
//
//===----------------------------------------------------------------------===//

#include "ClangMessages.h"
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/IR/DebugLoc.h>

namespace tsar {
msg::Location getLocation(
    const clang::SourceLocation &SLoc, const clang::SourceManager &SrcMgr) {
  msg::Location MsgLoc;
  MsgLoc[msg::Location::Line] = SrcMgr.getExpansionLineNumber(SLoc);
  MsgLoc[msg::Location::Column] = SrcMgr.getExpansionColumnNumber(SLoc);
  MsgLoc[msg::Location::MacroLine] = SrcMgr.getSpellingLineNumber(SLoc);
  MsgLoc[msg::Location::MacroColumn] = SrcMgr.getSpellingColumnNumber(SLoc);
  return MsgLoc;
}

msg::Location getLocation(llvm::DebugLoc &Loc) {
  assert(Loc && "Debug location must be valid!");
  msg::Location MsgLoc;
  MsgLoc[msg::Location::Line] = Loc.getLine();
  MsgLoc[msg::Location::Column] = Loc.getCol();
  MsgLoc[msg::Location::MacroLine] = Loc.getLine();
  MsgLoc[msg::Location::MacroColumn] = Loc.getCol();
  return MsgLoc;
}
}
