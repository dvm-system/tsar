//===- InterprocAttr.h - Interprocedural Attributes Deduction ---*- C++ -*-===//
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
// This file declares passes which perform interprocedural analysis in order to
// deduce some function and loop attributes.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Attributes.h"
#include "tsar/Transform/IR/Passes.h"
#include <bcl/utility.h>
#include <bcl/tagged.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/Pass.h>

#ifndef TSAR_INTERPROC_ATTR_H
#define TSAR_INTERPROC_ATTR_H

namespace llvm {
class Loop;

/// This pass uses functions which are called from a loop in order to deduce
/// loop attributes.
class LoopAttributesDeductionPass :
  public FunctionPass, private bcl::Uncopyable {

  template<class AttrT, unsigned N>
  using TaggedSet = bcl::tagged<SmallSet<AttrT, N>, AttrT>;
public:
  static char ID;
  LoopAttributesDeductionPass() : FunctionPass(ID) {
    initializeLoopAttributesDeductionPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void releaseMemory() override { mAttrs.clear(); }
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Returns true if a specified loop has a specified attribute.
  ///
  /// LLVM and SAPFOR attributes are supported.
  template<class KindT>
  bool hasAttr(const Loop &L, KindT Kind) {
    auto I = mAttrs.find(&L);
    return I != mAttrs.end() && I->second.get<KindT>().count(Kind);
  }
private:
  DenseMap<const Loop *, bcl::tagged_pair<
      TaggedSet<tsar::AttrKind, 2>,
      TaggedSet<llvm::Attribute::AttrKind, 2>>> mAttrs;
};
}

#endif//TSAR_INTERPRO_ATTR_H
