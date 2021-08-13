//===- SourceUnparserUtils.h - Utils For Source Info Unparser ---*- C++ -*-===//
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
// This file defines utility functions to generalize unparsing of metdata
// for different source languages.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SOURCE_UNPARSER_UTILS_H
#define TSAR_SOURCE_UNPARSER_UTILS_H

#include <llvm/ADT/SmallVector.h>

namespace llvm {
class raw_ostream;
class DominatorTree;
class CallBase;
class Module;
}

namespace tsar {
struct DIMemoryLocation;

/// Unparses the expression (in a specified language DWLang) and appends result
/// to a specified string, returns true on success.
bool unparseToString(unsigned DWLang,
  const DIMemoryLocation &Loc, llvm::SmallVectorImpl<char> &S,
  bool IsMinimal = true);

/// Unparses the expression (in a specified language DWLang) and prints result
/// to a specified stream returns true on success.
bool unparsePrint(unsigned DWLang,
  const DIMemoryLocation &Loc, llvm::raw_ostream &OS,
  bool IsMinimal = true);

/// Unparses the expression (in a specified language DWLang) and prints result
/// to the debug stream returns true on success.
bool unparseDump(unsigned DWLang, const DIMemoryLocation &Loc,
  bool IsMinimal = true);

/// Unparses callee and appends result to a specified string,
/// returns true on success.
bool unparseCallee(const llvm::CallBase &CB, llvm::Module &M,
  llvm::DominatorTree &DT, llvm::SmallVectorImpl<char> &S,
  bool IsMinimal = true);
}
#endif//TSAR_SOURCE_UNPARSER_UTILS_H
