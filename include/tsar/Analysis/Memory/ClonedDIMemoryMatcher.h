//=== ClonedDIMemoryMatcher.h - Original-to-Clone Memory Matcher *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file defines a pass to match metadata-level memory locations in
// an original module and its clone.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLONED_MEMORY_MATCHER_H
#define TSAR_CLONED_MEMORY_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Memory/BimapDIMemoryHandle.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <bcl/tagged.h>
#include <llvm/ADT/DenseMapInfo.h>

namespace llvm {
class MDNode;
}

namespace tsar {
/// Map from raw metadata-level memory representation to a memory location
/// in metadata-level alias tree.
using MDToDIMemoryMap = llvm::DenseMap<llvm::MDNode *, WeakDIMemoryHandle>;

/// This is a map from a memory location in original module to a cloned one.
class ClonedDIMemoryMatcher :
  public Bimap<
    bcl::tagged<BimapDIMemoryHandle<Origin, ClonedDIMemoryMatcher>, Origin>,
    bcl::tagged<BimapDIMemoryHandle<Clone, ClonedDIMemoryMatcher>, Clone>,
    DIMemoryMapInfo, DIMemoryMapInfo> {};

using OriginDIMemoryHandle = BimapDIMemoryHandle<Origin, ClonedDIMemoryMatcher>;
using CloneDIMemoryHandle = BimapDIMemoryHandle<Clone, ClonedDIMemoryMatcher>;

} // namespace tsar

namespace llvm {
/// Wrapper to allow server passes to determine relation between original
/// and cloned metadata-level memory locations.
using ClonedDIMemoryMatcherWrapper =
    AnalysisWrapperPass<tsar::ClonedDIMemoryMatcher>;

/// Create immutable storage for matched memory.
ImmutablePass *createClonedDIMemoryMatcherStorage();

/// Create a pass with a specified math between cloned MDNodes and original
/// DIMemory.
FunctionPass *
createClonedDIMemoryMatcher(const tsar::MDToDIMemoryMap &CloneToOriginal);

/// Create a pass with a specified math between cloned MDNodes and original
/// DIMemory.
FunctionPass *createClonedDIMemoryMatcher(tsar::MDToDIMemoryMap &&);

/// Initialize a pass to store matched memory.
void initializeClonedDIMemoryMatcherStoragePass(PassRegistry &);

/// Initialize a pass to access matched memory.
void initializeClonedDIMemoryMatcherWrapperPass(PassRegistry &);

/// Initialize a pass to match memory (original to cloned).
void initializeClonedDIMemoryMatcherPassPass(PassRegistry &);
} // namespace llvm

#endif // TSAR_CLONED_MEMORY_MATCHER_H
