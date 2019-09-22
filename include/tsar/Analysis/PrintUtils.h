//===- PrintUtils.h --------- Output Functions ------------------*- C++ -*-===//
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
// This file defines a set of output functions for various bits of information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_PRINT_H
#define TSAR_SUPPORT_PRINT_H

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/DebugLoc.h>

namespace llvm {
class raw_ostreap;
class LoopInfo;
}

namespace tsar {
/// \brief Print loop tree which is calculated by the LoopInfo pass.
///
/// \param [in] LI Information about natural loops identified
/// by the LoopInfor pass.
/// param [in] FilenameOnly If it is true only name of a file will be printed.
void printLoops(llvm::raw_ostream &OS, const llvm::LoopInfo &LI,
  bool FilenameOnly = false);

/// Return name of a file or full path to a file for a specified location.
/// If `FilenameOnly` is true only name of a file will be returned.
llvm::StringRef getFile(llvm::DebugLoc Loc, bool FilenameOnly = false);

/// This is similar to default DebugLoc::print() method, however, an output
/// depends on `FilenameOnly` parameter.
/// If `FilenameOnly` is true only name of a file will be printed.
void print(llvm::raw_ostream &OS, llvm::DebugLoc Loc, bool FilenameOnly = false);
}
#endif//TSAR_SUPPORT_PRINT_H
