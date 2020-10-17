//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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

#ifndef TSAR_ANALYSIS_CLANG_UTILS_H
#define TSAR_ANALYSIS_CLANG_UTILS_H

#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Loop;
class MDNode;
}

namespace tsar {
class DFRegionInfo;
struct MemoryMatchInfo;

/// Determine direction of induction if step is known.
llvm::Optional<bool> isGrowingInduction(llvm::Loop &L,
    const CanonicalLoopSet &CL, DFRegionInfo &DFI);

/// Determine direction of base induction variables for all canonical
/// loops in the nest. If direction is not known and `DirectionIfUnknown` is
/// specified assume the specified direction.
///
/// Do not process multiple sub-loops of a loop.
llvm::BitVector isGrowingInduction(llvm::Loop &Outermost, unsigned SizeOfNest,
    const CanonicalLoopSet &CL, DFRegionInfo &DFI,
    bool *DirectionIfUnknown = nullptr);

/// Search for base induction variables in a specified loop nest.
///
/// All loops in the nest have to be presented in a canonical loop form. We do
/// not process multiple sub-loops of a loop.
/// \return false if a number of processed loops is less than a size of the
/// nest.
bool getBaseInductionsForNest(llvm::Loop &Outermost, unsigned SizeOfNest,
    const CanonicalLoopSet &CL, const DFRegionInfo &RI,
    const MemoryMatchInfo &MM,
    llvm::SmallVectorImpl<std::pair<llvm::MDNode *, llvm::StringRef>>
        &Inductions);
}
#endif//TSAR_ANALYSIS_CLANG_UTILS_H
