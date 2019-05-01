//===-- ExpressionPropagation.h - Expression Propagation (Clang) -*- C++ -*===//
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
// This file defines a pass to replace the occurrences of variables with
// expressions which compute their values.
// Note, that propagation of array subranges is also supported, for example
// the following assignments can be processed '(*A)[5] = B[X]', where B is
// a 3-dimensional array.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_EXPR_PROPAGATION_H
#define TSAR_CLANG_EXPR_PROPAGATION_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Pass.h>

namespace tsar {
class TransformationContext;
struct DIMemoryLocation;
}

namespace llvm {
class Value;
class ConstantInt;
class DominatorTree;
class DataLayout;

class ClangExprPropagation : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  ClangExprPropagation() : FunctionPass(ID) {
    initializeClangExprPropagationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  /// Unparse replacement for a specified metadata-level candidate 'DIUse'.
  ///
  /// If `DIDef` is not specified then `Def` must be a constant for successful
  /// unparsing.
  bool unparseReplacement(const Value &Def, const tsar::DIMemoryLocation *DIDef,
    unsigned DWLang, const tsar::DIMemoryLocation &DIUse,
    SmallVectorImpl<char> &DefStr);

  const DataLayout *mDL = nullptr;
  DominatorTree *mDT = nullptr;
  tsar::TransformationContext *mTfmCtx = nullptr;
};
}

#endif//TSAR_CLANG_EXPR_PROPAGATION_H
