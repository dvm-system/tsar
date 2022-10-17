//===- VariableLocation.h -- Position of a variable in a code ------ C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements functions to get variable location in a source code from
// a metadata-level memory representation.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_UNPARSE_VARIABLE_LOCATION_H
#define TSAR_UNPARSE_VARIABLE_LOCATION_H

#include "tsar/Support/Tags.h"
#include <bcl/tagged.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Support/FileSystem/UniqueID.h>

namespace tsar {
class DIEstimateMemory;

/// Position of a variable in a source code.
using VariableLocationT = bcl::tagged_tuple<
    bcl::tagged<llvm::sys::fs::UniqueID, File>, bcl::tagged<unsigned, Line>,
    bcl::tagged<unsigned, Column>, bcl::tagged<std::string, Identifier>>;

llvm::Optional<VariableLocationT>
buildVariable(unsigned DWLang, const DIEstimateMemory &DIEM,
              llvm::SmallVectorImpl<char> &Path);

llvm::Optional<VariableLocationT> buildVariable(unsigned DWLang,
                                                const DIEstimateMemory &DIEM);
} // namespace tsar
#endif//TSAR_UNPARSE_VARIABLE_LOCATION_H
