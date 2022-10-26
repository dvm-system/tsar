//===--- GlobalOptions.h - Global Command Line Options ----------*- C++ -*-===//
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
// This file declares classes to store command line options that should be
// accessed from different places. For example, some options are necessary for
// multiple passes. A special ImmutableWrapperPass can be used to access such
// options.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GLOBAL_OPTIONS_H
#define TSAR_GLOBAL_OPTIONS_H

#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <vector>

namespace tsar {
struct GlobalOptions {
  enum IgnoreRedundantMemoryKind {
    IRMK_No = 0,
    IRMK_Strict,
    IRMK_Bounded,
    IRMK_Partial,
    IRMK_Weak
  };

  /// Print only names of files instead of full paths.
  bool PrintFilenameOnly = false;
  /// Print versions of all available tools.
  bool PrintToolVersion = false;
  /// Disallow unsafe integer type cast in analysis passes.
  bool IsSafeTypeCast = true;
  /// Assume that subscript expression is in bounds value of an array dimension.
  bool InBoundsSubscripts = false;
  /// Perform analysis of library functions.
  ///
  /// Note, that if this attribute is set to 'false' this does not mean that
  /// all library functions are not analyzed. Some analysis may be performed
  /// to ensure correctness of other results.
  bool AnalyzeLibFunc = true;
  /// Try to discard influence of redundant memory locations on the analysis
  /// results for other memory locations.
  IgnoreRedundantMemoryKind IgnoreRedundantMemory = IRMK_No;
  /// Try to analyze memory locations after unsafe transformations of related
  /// instructions.
  ///
  /// For example, 'instcombine' may remove some memory accesses and clobber
  /// metadata. So, some results may be lost. However analysis after
  /// transformation proposes more accurate results in some cases.
  bool UnsafeTfmAnalysis = false;
  /// Assume that functions are never called outside the analyzed module.
  bool NoExternalCalls = false;
  /// Disable function inlining which is useful for program analysis.
  bool NoInline = false;
  /// A function is only inlined if the number of memory accesses in the caller
  /// does not exceed this value.
  unsigned MemoryAccessInlineThreshold = 0;
  /// Passes to external analysis results which is used to clarify analysis.
  std::vector<std::string> AnalysisUse;
  /// Pass to profile which is used to measure possible optimization gain.
  std::string ProfileUse = "";
  /// List of object file names.
  std::vector<std::string> ObjectFilenames;
  /// Assumed weight of a function if a profile is not available.
  unsigned UnknownFunctionWeight = 1000000;
  /// Assumed weight of a builtin function if a profile is not available.
  unsigned UnknownBuiltinWeight = 20;
  /// If profile is available, this value specify the lowest weight of loops
  /// will be parallelized.
  unsigned LoopParallelThreshold = 0;
  /// List of regions which should be optimized.
  std::vector<std::string> OptRegions;
  /// This suffix should be add to transformed sources before extension.
  std::string OutputSuffix = "";
  /// Disable formatting of a source code after transformation.
  bool NoFormat = false;
};
}

namespace llvm {
/// Initialize a pass to access global command line options.
void initializeGlobalOptionsImmutableWrapperPass(PassRegistry &Registry);

/// Create a pass to access global command line options and
/// associates it with a specified list of options.
ImmutablePass * createGlobalOptionsImmutableWrapper(
  const tsar::GlobalOptions *Options);

/// Proposes access to global options.
class GlobalOptionsImmutableWrapper :
  public ImmutablePass, private bcl::Uncopyable {

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Creates pass with unspecified options.
  GlobalOptionsImmutableWrapper() : ImmutablePass(ID) {
    initializeGlobalOptionsImmutableWrapperPass(
      *PassRegistry::getPassRegistry());
  }

  /// Creates pass and associates it with a specified list of options.
  explicit GlobalOptionsImmutableWrapper(const tsar::GlobalOptions *Options) :
      ImmutablePass(ID), mOptions(Options) {
    assert(Options && "List of options must not be null!");
    initializeGlobalOptionsImmutableWrapperPass(
      *PassRegistry::getPassRegistry());
  }

  /// Returns a list of global options. List of options must be specified.
  const tsar::GlobalOptions & getOptions() const noexcept {
    assert(isSpecified() && "List of global options must be specified!");
    return *mOptions;
  }

  /// Set a list of global options, it must not be null.
  void setOptions(const tsar::GlobalOptions *Options) noexcept {
    assert(Options && "List of options must not be null!");
    mOptions = Options;
  }

  /// Returns true if a list of options is specified.
  bool isSpecified() const noexcept { return mOptions; }

private:
  const tsar::GlobalOptions *mOptions = nullptr;
};
}

#endif//TSAR_GLOBAL_OPTIONS_H
