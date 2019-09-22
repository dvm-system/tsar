//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
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
// This file defines helper methods for IR-level transformations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_TRANSFORM_IR_UTILS_H
#define TSAR_TRANSFORM_IR_UTILS_H

#include <llvm/ADT/SmallVector.h>

namespace llvm {
class DominatorTree;
class Instruction;
class Use;
}

namespace tsar {
/// Clone chain of instruction.
///
/// All clones instruction will be pushed to the `CloneList`. If some of
/// instructions can not be cloned the `CloneList` will not be updated.
/// Clones will not be inserted into IR. The lowest clone sill be the first
/// instruction which is pushed into the `CloneList`.
///
/// If `BoundInst` and `DT` is `nullptr` the full use-def chain will be cloned
/// (except Phi-nodes and alloca instructions). In case of Phi-nodes this method
/// returns false (cloning is impossible). The lowest instruction in the cloned
/// chain is `From`. If `BoundInst` and `DT` is specified instructions which
/// dominate BoundInst will not be cloned.
///
/// \return `true` on success, if instructions should not be cloned this
/// function also returns `true`.
bool cloneChain(llvm::Instruction *From,
  llvm::SmallVectorImpl<llvm::Instruction *> &CloneList,
  llvm::Instruction *BoundInst = nullptr, llvm::DominatorTree *DT = nullptr);

/// Traverses chains of operands of `From` instruction and performs a
/// a search for operands which do not dominate a specified `BoundInstr`.
///
/// This method if helpful when some instruction is cloned. Insertion
/// of a clone into IR should be performed accurately. Because operands must be
/// calculated before their uses. This functions can be used to determine
/// operands that should be fixed (for example, also cloned).
///
/// \return `true` if `From` does not dominate `BoundInst` (in this case NotDom
/// will be empty), otherwise return `false` (`NotDom` will contain all
/// instructions which have been found).
bool findNotDom(llvm::Instruction *From,
  llvm::Instruction *BoundInst, llvm::DominatorTree *DT,
  llvm::SmallVectorImpl<llvm::Use *> &NotDom);
}

#endif//TSAR_TRANSFORM_IR_UTILS_H
