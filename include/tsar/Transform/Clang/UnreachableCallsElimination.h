//=== DeadDeclsElimination.cpp - Dead Decls Elimination (Clang) --*- C++ -*===//
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
//===---------------------------------------------------------------------===//
//
// This file defines a pass to eliminate unreachable calls in a source code.
//
//===---------------------------------------------------------------------===//

#ifndef TSAR_CLANG_UNREACHABLE_CALLS_H
#define TSAR_CLANG_UNREACHABLE_CALLS__H


#include <bcl/utility.h>
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/Pass.h>
#include <llvm/ProfileData/Coverage/CoverageMapping.h>
#include <vector>


namespace llvm {
/// This per-function pass performs source-level elimination of
/// unreachable calls.
class ClangUnreachableCallsElimination : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangUnreachableCallsElimination();

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_CLANG_UNREACHABLE_CALLS__H
