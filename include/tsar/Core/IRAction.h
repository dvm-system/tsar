//===--- IRAction.h --------- TSAR IR Action --------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements an action to process multi-file project. It loads IR
// from a file, parses corresponding sources and attach them to the
// transformation context.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_IR_ACTION_H
#define TSAR_IR_ACTION_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <string>

namespace clang {
namespace tooling {
class CompilationDatabase;
}
} // namespace clang

namespace tsar {
class QueryManager;

int executeIRAction(
    llvm::StringRef ToolName, llvm::ArrayRef<std::string> Sources,
    QueryManager &QM,
    const clang::tooling::CompilationDatabase *Compilations = nullptr);
}
#endif//TSAR_IR_ACTION_H
