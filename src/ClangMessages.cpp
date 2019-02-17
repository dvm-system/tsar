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

namespace tsar {
msg::Location getLocation(
    clang::SourceLocation &SLoc, clang::SourceManager &SrcMgr) {
  msg::Location MsgLoc;
  MsgLoc[msg::Location::Line] = SrcMgr.getExpansionLineNumber(SLoc);
  MsgLoc[msg::Location::Column] = SrcMgr.getExpansionColumnNumber(SLoc);
  MsgLoc[msg::Location::MacroLine] = SrcMgr.getSpellingLineNumber(SLoc);
  MsgLoc[msg::Location::MacroColumn] = SrcMgr.getSpellingColumnNumber(SLoc);
  return MsgLoc;
}
}
