//===- APCContext.h - Opaque implementation of APC context ---  -*- C++ -*-===//
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
// This file declares APCContextImpl, opaque implementation of APCContext.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_APC_CONTEXT_IMPL_H
#define TSAR_APC_CONTEXT_IMPL_H

#include "tsar_utility.h"
#include <apc/Distribution/Array.h>
#include <apc/GraphCall/graph_calls.h>
#include <apc/GraphLoop/graph_loops.h>
#include <apc/Utils/AstWrapper.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/ValueHandle.h>

namespace tsar {
struct APCContextImpl {
  using ParallelRegionPool = std::vector<std::unique_ptr<ParallelRegion>>;

  using LoopPool = std::vector<std::unique_ptr<LoopGraph>>;
  using LoopMap = llvm::DenseMap<ObjectID, LoopGraph *>;

  using ArrayMap = llvm::DenseMap<ObjectID, std::unique_ptr<Distribution::Array>>;

  using FunctionMap = llvm::DenseMap<llvm::Function *, std::unique_ptr<FuncInfo>>;

  ParallelRegionPool ParallelRegions;

  LoopPool OuterLoops;
  LoopMap Loops;

  ArrayMap Arrays;

  FunctionMap Functions;
};
}

#endif//TSAR_APC_CONTEXT_IMPL_H
