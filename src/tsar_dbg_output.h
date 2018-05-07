//===---- tsar_dbg_output.h - Output functions for debugging ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a set of output functions for various bits of information.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DBG_OUTPUT_H
#define TSAR_DBG_OUTPUT_H

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>

namespace llvm {
class DominatorTree;
class Function;
class LoopInfo;
class MemoryLocation;
class raw_ostream;
class Value;
}

namespace tsar {
struct DIMemoryLocation;
class DIMemory;

/// \brief Prints information available from a source code for the
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

/// Prints information available from a source code for the specified memory
/// location and its size (<location, size>).
void printLocationSource(llvm::raw_ostream &O, const llvm::MemoryLocation &Loc,
    const llvm::DominatorTree *DT = nullptr);

/// Prints source level representation (in a specified language 'DWLang')
/// of a debug level memory location.
void printDILocationSource(unsigned DWLang,
    const DIMemoryLocation &Loc, llvm::raw_ostream &O);

/// Prints source level representation (in a specified language 'DWLang')
/// of a debug level memory location.
void printDILocationSource(unsigned DWLang,
    const DIMemory &Loc, llvm::raw_ostream &O);

/// \brief Print description of a type from a source code.
///
/// \param [in] DITy Meta information for a type.
void printDIType(llvm::raw_ostream &o, const llvm::DITypeRef &DITy);

/// \brief Print description of a variable from a source code.
///
/// \param [in] DIVar Meta information for a variable.
void printDIVariable(llvm::raw_ostream &o, llvm:: DIVariable *DIVar);

/// \brief Prints loop tree which is calculated by the LoopInfo pass.
///
/// \param [in] LI Information about natural loops identified
/// by the LoopInfor pass.
/// param [in] FilenameOnly If it is true only name of a file will be printed.
void printLoops(llvm::raw_ostream &OS, const llvm::LoopInfo &LI,
  bool FilenameOnly = false);

/// Returns name of a file or full path to a file for a specified location.
/// If `FilenameOnly` is true only name of a file will be returned.
inline llvm::StringRef getFile(llvm::DebugLoc Loc, bool FilenameOnly = false) {
  auto *Scope = llvm::cast<llvm::DIScope>(Loc.getScope());
  auto Filename = Scope->getFilename();
  if (FilenameOnly) {
    Filename.consume_front(Scope->getDirectory());
    Filename = Filename.ltrim("/\\");
  }
  return Filename;
}

/// This is similar to default DebugLoc::print() method, however, an output
/// depends on `FilenameOnly` parameter.
/// If `FilenameOnly` is true only name of a file will be printed.
void print(llvm::raw_ostream &OS, llvm::DebugLoc Loc, bool FilenameOnly = false);
}

#endif//TSAR_DBG_OUTPUT_H
