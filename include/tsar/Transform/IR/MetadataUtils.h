//===----- MetadataUtils.h - Utils for exploring metadata -------*- C++ -*-===//
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
// This file defines helpful functions to create and access metadata.
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Metadata.h>
#include <utility>

#ifndef TSAR_METADATA_UTILS_H
#define TSAR_METADATA_UTILS_H

namespace tsar {
/// \brief Add description of a global value into the list of a named metadata.
///
/// This description will contain:
/// - string with a value `ValueKind`;
/// - metadata which refers to the global value;
/// - metadata from MDs.
/// A global value will be also marked with `NamedMD` metadata which
/// refers to this description.
void addNameDAMetadata(llvm::GlobalObject &GV, llvm::StringRef NamedMD,
  llvm::StringRef ValueKind, llvm::ArrayRef<llvm::Metadata *> MDs = {});

/// \brief Erase from a specified module `M` a global object which is identified
/// by a metadata `MD` in a list of named metadata `NamedMD`.
///
/// This function searches for metadata which contains `MDString` equal to`MD`.
/// If there are no such object or metadata, do nothing.
/// \return Index of operand which points to a removed object in the `MD`
/// metadata.
llvm::Optional<unsigned> eraseFromParent(llvm::Module &M,
  llvm::StringRef NamedMD, llvm::StringRef MD);

/// Returns metadata from a named list of metdata which contains a specified
/// string.
llvm::MDNode * getMDOfKind(llvm::NamedMDNode &NamedMD, llvm::StringRef Kind);

/// Returns operand which is a llvm::Value and which can be converted
/// to a specified type.
template<class Ty>
static std::pair<Ty *, unsigned> extractMD(llvm::MDNode &MD) {
  for (unsigned I = 0, E = MD.getNumOperands(); I != E; ++I)
    if (auto *V = llvm::mdconst::dyn_extract_or_null<Ty>(MD.getOperand(I)))
      return std::make_pair(V, I);
  return std::make_pair(nullptr, 0);
}
}

#endif//TSAR_METADATA_UTILS_H
