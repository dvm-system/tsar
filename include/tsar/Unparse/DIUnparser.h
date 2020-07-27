//===--- DIUnparser.h -------- Debug Info Unparser --------------*- C++ -*-===//
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
// This file defines unparser to print low-level LLVM IR in a more readable
// form, similar to high-level language.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DIUNPARSER_H
#define TSAR_DIUNPARSER_H

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace llvm {
class DominatorTree;
class GEPOperator;
class raw_ostream;
class Value;
}

namespace tsar {
/// \brief This class performs unparsing of a value and represents it in
/// a high-level form.
///
/// Cast operations are not supported because after such operations DIType
/// of an expression becomes unknown.
///
/// TODO (kaniandr@gmail.com): add support for union types, enumerations, etc.
/// TODO (kaniandr@gmail.com): add support for arithmetical expressions.
class DIUnparser {
public:
  /// Creates unparser for a specified expression.
  explicit DIUnparser(const llvm::Value *Expr,
      const llvm::DominatorTree *DT = nullptr) noexcept :
      mExpr(Expr), mDT(DT) {
    assert(Expr && "Memory location must not be null!");
  }

  /// Returns expression that should be unparsed.
  const llvm::Value * getValue() const noexcept { return mExpr; }

  /// Unparses the expression and appends result to a specified string,
  /// returns true on success.
  bool toString(llvm::SmallVectorImpl<char> &Str) {
    return unparse(mExpr, Str) ? endDITypeIfNeed(Str), true : false;
  }

  /// Unparses the expression and prints result to a specified stream
  /// returns true on success.
  bool print(llvm::raw_ostream &OS);

  /// Unparses the expression and prints result to a debug stream,
  /// returns true on success.
  bool dump();

private:
  /// \brief Add omitted subscripts for defer evaluated arrays and pointers.
  ///
  /// It is not always known how many subscripts should be added because it
  /// depends of unparsed location, for example was it an element X[1][2]
  /// or a full dimension X[1]. In this case `[?]:0..N` will be added, where
  /// N is a max number of dimensions.
  void endDITypeIfNeed(llvm::SmallVectorImpl<char> &Str);

  /// Unparses a specified expression and appends result to a specified string,
  /// returns true on success.
  bool unparse(const llvm::Value *Expr, llvm::SmallVectorImpl<char> &Str);

  /// Unparses `getelementptr` instructions, return true on success.
  bool unparse(const llvm::GEPOperator *GEP, llvm::SmallVectorImpl<char> &Str);

  const llvm::DominatorTree *mDT;
  const llvm::Value *mExpr;
  llvm::DIType *mDIType = nullptr;
  const llvm::Value *mLastAddressChange = nullptr;
  bool mIsDITypeEnd = true;
};

/// Unparses the expression and appends result to a specified string,
/// returns true on success.
inline bool unparseToString(llvm::SmallVectorImpl<char> &S,
    const llvm::Value *V, const llvm::DominatorTree *DT = nullptr) {
  DIUnparser U(V, DT);
  return U.toString(S);
}

/// Unparses the expression and prints result to a specified stream
/// returns true on success.
inline bool unparsePrint(llvm::raw_ostream &OS, const llvm::Value *V,
    const llvm::DominatorTree *DT = nullptr) {
  DIUnparser U(V, DT);
  return U.print(OS);
}

/// Unparses the expression and prints result to a debug stream,
/// returns true on success.
inline bool unparseDump(const llvm::Value *V,
    const llvm::DominatorTree *DT = nullptr) {
  DIUnparser U(V, DT);
  return U.dump();
}
}
#endif//TSAR_DIUNPARSER_H
