//===-- ClangMessages.h ----- JSON Messages (Clang) -------------*- C++ -*-===//
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
// This defines Clang-based messages for server-client interaction,
// where server is TSAR.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_CLANG_MESSAGES_H
#define TSAR_CLANG_MESSAGES_H

#include "Messages.h"

namespace clang {
class SourceLocation;
class SourceManager;
}

namespace llvm {
class DebugLoc;
}

namespace tsar {
/// Converts a specified location to JSON.
msg::Location getLocation(
  const clang::SourceLocation &SLoc, const clang::SourceManager &SrcMgr);

/// Convert a specified location to JSON.
msg::Location getLocation(
  llvm::DebugLoc &Loc, const clang::SourceManager &SrcMgr);
}
#endif//TSAR_CLANG_MESSAGES_H
