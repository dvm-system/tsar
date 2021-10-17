//===- RedundantEdgeElimination.h - Redundant Edge Elimination --*- C++ -*-===//
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
// This file defines a pass to replace the always true conditional branch
// instructions with an unconditional branch instruction.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_REDUNDANT_EDGE_ELIMINATION_H
#define TSAR_REDUNDANT_EDGE_ELIMINATION_H

#include "tsar/Transform/IR/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
class RedundantEdgeEliminationPass :
    public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  RedundantEdgeEliminationPass() : FunctionPass(ID) {
    initializeRedundantEdgeEliminationPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
};
}
#endif//TSAR_REDUNDANT_EDGE_ELIMINATION_H