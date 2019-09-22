//===- Passes.h - Create and Initialize Parse Passes (Clang) ----*- C++ -*-===//
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
// It contains declarations of functions that initialize and create an instances
// of TSAR passes which are necessary to parse C programs.
// Declarations of appropriate methods for an each new pass should
// be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PARSE_CLANG_PASSES_H
#define TSAR_PARSE_CLANG_PASSES_H

namespace tsar {
struct ASTImportInfo;
}

namespace llvm {
class PassRegistry;
class ImmutablePass;

/// Initialize all passes to parse C programs.
void initializeClangParse(PassRegistry &Registry);

/// Initialize a pass to obtain access to the import process information.
void initializeImmutableASTImportInfoPassPass(PassRegistry &Registry);

/// Create a pass to obtain access to the import process information.
ImmutablePass * createImmutableASTImportInfoPass(
  const tsar::ASTImportInfo &Info);
}
#endif//TSAR_PARSE_CLANG_PASSES_H
