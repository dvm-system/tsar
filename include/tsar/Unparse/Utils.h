//===- Utils.h -------------- Output Functions ------------------*- C++ -*-===//
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

#ifndef TSAR_UNPARSE_UTILS_H
#define TSAR_UNPARSE_UTILS_H

namespace llvm {
class DominatorTree;
class MemoryLocation;
class Value;
class raw_ostream;
class DIType;
class DIVariable;
}

namespace tsar {
struct MemoryLocationRange;
struct DIMemoryLocation;
class EstimateMemory;
class DIMemory;

/// Print information available from a source code for the
/// specified memory location.
///
/// \pre At this moment location can be represented as a sequence of 'load' or
/// 'getelementptr' instructions ending 'alloca' instruction or global variable
/// or other values marked with llvm.dbg.declare or llvm.dbg.value.
/// Note that `DT` is optional parameters but it is necessary to determine
/// appropriate llvm.dbg.value instruction.
/// \par Example
/// \code
///    ...
/// 1: int *p;
/// 2: *p = 5;
/// \endcode
/// \code
/// %p = alloca i32*, align 4
/// %0 = load i32*, i32** %p, align 4
/// \endcode
/// If debug information is available the result for
/// %0 will be p[0] otherwise it will be *(%p = alloca i32*, align 4).
void printLocationSource(llvm::raw_ostream &O, const llvm::Value *Loc,
    const llvm::DominatorTree *DT = nullptr);

/// Print information available from a source code for the
/// specified memory location.
inline void printLocationSource(llvm::raw_ostream &O, const llvm::Value &Loc,
    const llvm::DominatorTree *DT = nullptr) {
  printLocationSource(O, &Loc, DT);
}

/// Print information available from a source code for the specified memory
/// location and its size (<location, size>).
void printLocationSource(llvm::raw_ostream &O, const llvm::MemoryLocation &Loc,
    const llvm::DominatorTree *DT = nullptr);

/// Print information available from a source code for the specified memory
/// location and its size (<location, size>).
void printLocationSource(llvm::raw_ostream &O, const MemoryLocationRange &Loc,
    const llvm::DominatorTree *DT = nullptr, bool IsDebug = false);

/// Print information available from a source code for the specified memory
/// location and its size (<location, size>).
void printLocationSource(llvm::raw_ostream &O, const EstimateMemory &EM,
    const llvm::DominatorTree *DT = nullptr);

/// Print source level representation (in a specified language 'DWLang')
/// of a debug level memory location.
void printDILocationSource(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::raw_ostream &O);

/// Print source level representation (in a specified language 'DWLang')
/// of a debug level memory location.
void printDILocationSource(unsigned DWLang,
    const DIMemory &Loc, llvm::raw_ostream &O);

/// Print description of a type from a source code.
///
/// \param [in] DITy Meta information for a type.
void printDIType(llvm::raw_ostream &o, const llvm::DIType *DITy);

/// Print description of a variable from a source code.
///
/// \param [in] DIVar Meta information for a variable.
void printDIVariable(llvm::raw_ostream &o, llvm:: DIVariable *DIVar);
}

#endif//TSAR_UNPARSE_UTILS_H
