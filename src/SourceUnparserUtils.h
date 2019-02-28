//===- SourceUnparserUtils.h - Utils For Source Info Unparser ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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
class CallSite;
class Module;
}

namespace tsar {
struct DIMemoryLocation;

/// Unparses the expression (in a specified language DWLang) and appends result
/// to a specified string, returns true on success.
bool unparseToString(unsigned DWLang,
  const DIMemoryLocation &Loc, llvm::SmallVectorImpl<char> &S);

/// Unparses the expression (in a specified language DWLang) and prints result
/// to a specified stream returns true on success.
bool unparsePrint(unsigned DWLang,
  const DIMemoryLocation &Loc, llvm::raw_ostream &OS);

/// Unparses the expression (in a specified language DWLang) and prints result
/// to the debug stream returns true on success.
bool unparseDump(unsigned DWLang, const DIMemoryLocation &Loc);

/// Unparses callee and appends result to a specified string,
/// returns true on success.
bool unparseCallee(const llvm::CallSite &CS, llvm::Module &M,
  llvm::DominatorTree &DT, llvm::SmallVectorImpl<char> &S);
}
#endif//TSAR_SOURCE_UNPARSER_UTILS_H
