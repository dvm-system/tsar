//===--- Utils.h -------------- Memory Utils ---------------------*- C++ -*===//
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MEMORY_UTILS_H
#define TSAR_MEMORY_UTILS_H

namespace llvm {
class Instruction;
class Loop;
class ScalarEvolution;
class SCEV;
class TargetLibraryInfo;
}

namespace tsar {
class AliasTree;
class DefUseSet;
template<class GraphType> class SpanningTreeRelation;

/// Return 'true' if values in memory accessed in a specified instruction 'I'
/// is region invariant.
bool isRegionInvariant(llvm::Instruction &I, llvm::TargetLibraryInfo &TLI,
  const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);

/// Return 'true' if a specified expression 'Expr' is loop invariant. If loop is
/// null then the whole function will be evaluated.
bool isLoopInvariant(const llvm::SCEV *Expr, const llvm::Loop *L,
  llvm::TargetLibraryInfo &TLI, llvm::ScalarEvolution &SE,
  const DefUseSet &DUS, const AliasTree &AT,
  const SpanningTreeRelation<const AliasTree *> &STR);
}
#endif//TSAR_MEMORY_UTILS_H
