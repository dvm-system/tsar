//=== tsar_memory_matcher.h - High and Low Level Memory Matcher -*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

#include "AnalysisWrapperPass.h"
#include "tsar_bimap.h"
#include "tsar_utility.h"
#include <bcl/utility.h>
#include <set>

namespace clang {
class FuncDecl;
class VarDecl;
}

namespace llvm {
class Value;
}

namespace tsar {
/// This provides access to results of a memory match analysis.
struct MemoryMatchInfo : private bcl::Uncopyable {
  typedef tsar::Bimap<
    bcl::tagged<clang::VarDecl*, tsar::AST>,
    bcl::tagged<llvm::Value *, tsar::IR>> MemoryMatcher;

  typedef std::set<clang::VarDecl *> MemoryASTSet;

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
