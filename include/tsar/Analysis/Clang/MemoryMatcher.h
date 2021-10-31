//===- MemoryMatcher.h - High and Low Level Memory Matcher ------*- C++ -*-===//
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
// Classes and functions from this file match variables in a source high-level
// code and appropriate allocas or globals in low-level LLVM IR. This file
// implements classes to access results of such analysis.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_MATCHER_H
#define TSAR_MEMORY_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/ADT/ListBimap.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>

namespace clang {
class FuncDecl;
class VarDecl;
}

namespace llvm {
class Value;
}

namespace tsar {
/// This provides access to results of a memory match analysis.
///
/// Note that matcher contains canonical declarations (Decl::getCanonicalDecl).
struct MemoryMatchInfo : private bcl::Uncopyable {
  typedef tsar::ListBimap<
    bcl::tagged<clang::VarDecl*, tsar::AST>,
    bcl::tagged<llvm::Value *, tsar::IR>> MemoryMatcher;

  typedef llvm::DenseSet<clang::VarDecl *> MemoryASTSet;

  /// Memory matcher for the last analyzed module.
  MemoryMatcher Matcher;

  /// Unmatched memory in AST.
  MemoryASTSet UnmatchedAST;
};
}

namespace llvm {
/// Wrapper to access results of a memory matcher pass.
using MemoryMatcherImmutableWrapper = AnalysisWrapperPass<tsar::MemoryMatchInfo>;
}
#endif//TSAR_MEMORY_MATCHER_H
