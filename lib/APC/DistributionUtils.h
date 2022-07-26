//===- DistributionUtils.h - Utils to Process Distribution Rules *- C++ -*-===//
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
// This file contains useful functions to explore distribution rules.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DISTRIBUTION_UTILS_H
#define TSAR_DISTRIBUTION_UTILS_H

#include "tsar/APC/APCContext.h"
#include <llvm/ADT/SmallVector.h>

namespace tsar {
/// Convert alignment rule to rules for each dimension of a template.
///
/// Initially, there is list of alignment rules which attached to a dimension of
/// an array.
/// \return Array of size equal to number of template dimensions. For each
/// dimension index of corresponding alignment rule in
/// apc::AlignRule::alinRuleWith will be set.
/// If template dimension should not be aligned it will be set to a number of
/// template dimensions.
llvm::SmallVector<std::size_t, 8> extractTplDimsAlignmentIndexes(
    const apc::AlignRule &AR);

/// Convert a specified message to a string representation.
void messageToString(const apc::Messages &Msg,
                     llvm::SmallVectorImpl<char> &Out);

/// Print a specified message to a string representation.
void messagePrint(const llvm::Twine &File, const apc::Messages &Msg,
                  llvm::raw_ostream &OS);
}
#endif//TSAR_DISTRIBUTION_UTILS_H
