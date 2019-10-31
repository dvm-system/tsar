//===--- AnalysisWrapperPass.h - Analysis Results Wrapper -------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file implements a wrapper to simplify access to results of analysis
// passes in case of wrapper and analysis pass are executed by different
// pass managers.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_WRAPPER_PASS_H
#define TSAR_ANALYSIS_WRAPPER_PASS_H

#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
/// \brief This wrapper provides access to a pass which has been executed early.
///
/// For example, this is useful to access result of a module pass from a
/// function pass manager. In this case module pass must be executed by
/// an outer pass manager. Then a wrapper can be used to access this pass.
/// It should be initialized (for example, using a pass provider) by a reference
/// to the module pass or its result.
template<class AnalysisTy, class WrapperTy = ImmutablePass>
class AnalysisWrapperPass : public WrapperTy, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  AnalysisWrapperPass() : WrapperTy(ID) { }

  /// Returns an underlying pass.
  AnalysisTy & get() noexcept {
    assert(mAnalysis && "Wrapped pass must be specified!");
    return *mAnalysis;
  }

  /// Returns an underlying pass.
  const AnalysisTy & get() const noexcept {
    assert(mAnalysis && "Wrapped pass must be specified!");
    return *mAnalysis;
  }

  /// Initializes this wrapper.
  void set(AnalysisTy &P) noexcept { mAnalysis = &P; }

  /// Returns true if this wrapper is initialized.
  operator bool() const noexcept { return mAnalysis != nullptr; }

  AnalysisTy & operator*() noexcept { return get(); }
  AnalysisTy * operator->() noexcept { return &operator *(); }
  const AnalysisTy & operator*() const noexcept { return get(); }
  const AnalysisTy * operator->() const noexcept { return &operator *(); }
private:
  AnalysisTy *mAnalysis = nullptr;
};
}
#endif//TSAR_ANALYSIS_WRAPPER_PASS_H
