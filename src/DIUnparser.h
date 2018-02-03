//===--- DIUnparser.h -------- Debug Info Unparser --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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
class GEPOperator;
class raw_ostream;
class Value;
}

namespace tsar {
/// \brief This class performs unparsing of a value and represents it in
/// a high-level form.
///
/// TODO (kaniandr@gmail.com): add support for union types, enumerations, etc.
/// TODO (kaniandr@gmail.com): add support for bitcast, addrspacecast,
/// inttoptr instructions and other cast operations.
/// TODO (kaniandr@gmail.com): add support for arithmetical expressions.
class DIUnparser {
public:
  /// Creates unparser for a specified expression.
  explicit DIUnparser(const llvm::Value *Expr) noexcept : mExpr(Expr) {
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

  const llvm::Value *mExpr;
  llvm::DITypeRef mDIType;
  const llvm::Value *mLastAddressChange = nullptr;
  bool mIsDITypeEnd = true;
};

/// Unparses the expression and appends result to a specified string,
/// returns true on success.
inline bool unparseToString(
    const llvm::Value *V, llvm::SmallVectorImpl<char> &S) {
  DIUnparser U(V);
  return U.toString(S);
}

/// Unparses the expression and prints result to a specified stream
/// returns true on success.
inline bool unparsePrint(const llvm::Value *V, llvm::raw_ostream &OS) {
  DIUnparser U(V);
  return U.print(OS);
}

/// Unparses the expression and prints result to a debug stream,
/// returns true on success.
inline bool unparseDump(const llvm::Value *V) {
  DIUnparser U(V);
  return U.dump();
}
}
#endif//TSAR_DIUNPARSER_H
