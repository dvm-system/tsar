//===--- SourceUnparser.h --- Source Info Unparser --------------*- C++ -*-===//
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
// This file defines unparser to print metadata objects as constructs of
// an appropriate high-level language.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SOURCE_UNPARSER_H
#define TSAR_SOURCE_UNPARSER_H

#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Compiler.h>
#include <vector>

namespace llvm {
class MDNode;
class DIType;
class raw_ostream;
}

namespace tsar {
/// This is implement unparser to print metadata objects as a set of tokens.
///
/// An unparsed object will be represented as a list of tokens. There are two
/// type of lists: prefix and suffix. The result is prefix + variable + suffix.
/// Some of tokens in suffix have additional value (constants, identifiers).
/// This value a stored in an appropriate collections according to order
/// of tokens with value. A name of the variable is a first value in the list
/// of identifiers. Subscript expressions are represented as a list of constants
/// (may be with a sign represented as an appropriate token) between begin and
/// end tokens.
/// \attention This class does not check correctness of metadata object. In case
/// of incorrect object behavior is undefined.
class SourceUnparserImp {
public:
  /// List of available tokens.
  enum Token : uint8_t {
    FIRST_TOKEN = 0,
    TOKEN_ADDRESS = FIRST_TOKEN,
    TOKEN_DEREF,
    TOKEN_IDENTIFIER,
    TOKEN_UCONST,
    TOKEN_UNKNOWN,
    TOKEN_SEPARATOR,
    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_PARENTHESES_LEFT,
    TOKEN_PARENTHESES_RIGHT,
    TOKEN_SUBSCRIPT_BEGIN,
    TOKEN_SUBSCRIPT_END,
    TOKEN_FIELD,
    TOKEN_CAST_TO_ADDRESS,
    LAST_TOKEN = TOKEN_CAST_TO_ADDRESS,
    INVALID_TOKEN,
    TOKEN_NUM = INVALID_TOKEN
  };

  using TokenList = llvm::SmallVector<Token, 8>;
  using IdentifierList = std::vector<llvm::MDNode *>;
  using UnsignedConstList = llvm::SmallVector<uint64_t, 4>;

  ///\brief Creates unparser for a specified expression.
  ///
  /// \param [in] Loc Unparsed expression.
  /// \param [in] IsForwardDim Direction of dimension of arrays in memory.
  /// For example, `true` in case of C and `false` in case of Fortran.
  /// \param [in] IsMinimal If 'true', stop unparsing as soon as pointer to the
  /// beginning of memory location is unparsed and size of pointee type is equal
  /// to a size of location. For example, minimal unparsing of `A[0]` (if type
  /// is `int A[1]`) leads to `A`.
  SourceUnparserImp(const DIMemoryLocation &Loc, bool IsForwardDim,
      bool IsMinimal = true) noexcept :
    mLoc(Loc), mIsForwardDim(IsForwardDim), mIsMinimal(IsMinimal) {
    assert(Loc.isValid() && "Invalid memory location!");
  }

  /// Returns expression that should be unparsed.
  DIMemoryLocation getValue() const noexcept { return mLoc; }

  /// Returns suffix which succeeds the variable name.
  const TokenList & getSuffix() const noexcept { return mSuffix; }

  /// Returns reversed prefix which precedes the variable name.
  const TokenList & getReversePrefix() const noexcept { return mReversePrefix; }

  /// \brief Returns list of identifiers.
  ///
  /// Each identifier is a MDNode which has a name, for example it may be
  /// DIVariable * (name of variables) or DIDerivedType * (name of a member of
  /// an aggregate type). The getName() method returns name of an identifier
  /// in this list.
  const IdentifierList & getIdentifiers() const noexcept { return mIdentifiers; }

  /// Returns list of unsigned constants.
  const UnsignedConstList & getUConsts() const noexcept { return mUConsts; }

  /// Returns priority of operation which is associated with a specified token.
  unsigned getPriority(Token T)  const noexcept {
    switch (T) {
    case TOKEN_ADD: case TOKEN_SUB: return 0;
    case TOKEN_DEREF: case TOKEN_ADDRESS: case TOKEN_CAST_TO_ADDRESS: return 1;
    case TOKEN_SUBSCRIPT_BEGIN: case TOKEN_SUBSCRIPT_END:
    case TOKEN_SEPARATOR: case TOKEN_FIELD: return 2;
    case TOKEN_IDENTIFIER: case TOKEN_UCONST: case TOKEN_UNKNOWN: return 3;
    case TOKEN_PARENTHESES_LEFT: case TOKEN_PARENTHESES_RIGHT: return 4;
    }
    llvm_unreachable("Unknown token!");
    return 0;
  }

  /// Returns name of an identifier from getIdentifiers() list.
  llvm::StringRef getName(const llvm::MDNode &Identifier) const;

  /// Performs unparsing.
  bool unparse();

private:
  /// Clear all lists and drop other values.
  void clear() {
    mReversePrefix.clear();
    mSuffix.clear();
    mIdentifiers.clear();
    mUConsts.clear();
    mIsAddress = false;
    mCurrType = nullptr;
    mLastOpPriority = 0;
  }

  bool unparse(uint64_t Offset, bool IsPositive);

  /// Unprses dwarf::DW_OP_deref.
  bool unparseDeref();

  /// This ignores mCurrType, converts currently unparsed expression to
  /// address unit and appends offset.
  bool unparseAsScalarTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_structure_type
  /// or dwarf::DW_TAG_class_type.
  bool unparseAsStructureTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_union_type.
  bool unparseAsUnionTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_array_type.
  bool unparseAsArrayTy(uint64_t Offset, bool IsPositive);

  /// This is called when mCurrType is dwarf::DW_TAG_pointer_type.
  bool unparseAsPointerTy(uint64_t Offset, bool IsPositive);

  /// Update priority of the last operation and add parentheses if necessary.
  void updatePriority(Token Current, Token Next) {
    if (mLastOpPriority < getPriority(Current)) {
      mReversePrefix.push_back(TOKEN_PARENTHESES_LEFT);
      mSuffix.push_back(TOKEN_PARENTHESES_RIGHT);
    }
    mLastOpPriority = getPriority(Next);
  }

  /// Add a specified number of subscript expressions to already unparsed
  /// expression.
  void addUnknownSubscripts(unsigned Dims);

  DIMemoryLocation mLoc;
  bool mIsMinimal;
  bool mIsForwardDim;
  TokenList mReversePrefix;
  TokenList mSuffix;
  IdentifierList mIdentifiers;
  UnsignedConstList mUConsts;

  /// \brief Currently unparsed type.
  ///
  ///If it conflicts with unparsed expression it
  /// will be ignored and set to nullptr. In this case all remains offsets will
  /// be appended as offsets in bytes (see unparseAsScalarTy()).
  llvm::DIType *mCurrType = nullptr;

  /// \brief If this set to true then result of already unparsed expression is
  /// an address independent of the mCurrType value.
  ///
  /// Let us consider an example, where unparsed expression is X and offset in
  /// address units is N. If mIsAddress = true then result will be
  /// (char *)X + N otherwise it will be (char *)&X + N.
  bool mIsAddress = false;

  /// \brief Priority of the last operation in already unparsed expression.
  ///
  /// This is used to investigate is it necessary to use parenthesis for
  /// the next operation.
  unsigned mLastOpPriority = 0;
};

template<class Unparser>
class SourceUnparser : public SourceUnparserImp {
public:
  ///\brief Creates unparser for a specified expression.
  ///
  /// \param [in] Loc Unparsed expression.
  /// \param [in] IsForwardDim Direction of dimension of arrays in memory.
  /// For example, `true` in case of C and `false` in case of Fortran.
  /// \param [in] IsMinimal If 'true', stop unparsing as soon as pointer to the
  /// beginning of memory location is unparsed and size of pointee type is equal
  /// to a size of location. For example, minimal unparsing of `A[0]` (if type
  /// is `int A[1]`) leads to `A`.
  SourceUnparser(const DIMemoryLocation &Loc, bool IsForwardDim,
      bool IsMinimal = true) noexcept :
    SourceUnparserImp(Loc, IsForwardDim, IsMinimal) {}

  /// Unparses the expression and appends result to a specified string,
  /// returns true on success.
  bool toString(llvm::SmallVectorImpl<char> &Str) {
    if (!unparse())
      return false;
    for (auto T : llvm::make_range(
         getReversePrefix().rbegin(), getReversePrefix().rend()))
      appendToken(T, false, Str);
    assert(!getIdentifiers().empty() && "At least one identifier must be set!");
    auto IdentItr = getIdentifiers().begin();
    auto Id = getName(**IdentItr);
    Str.append(Id.begin(), Id.end()), ++IdentItr;
    auto UConstItr = getUConsts().begin();
    bool IsSubscript = false;
    for (auto T : getSuffix()) {
      if (T == TOKEN_SUBSCRIPT_BEGIN)
        IsSubscript = true, beginSubscript(Str);
      else if (T == TOKEN_SUBSCRIPT_END)
        IsSubscript = false, endSubscript(Str);
      else if (T == TOKEN_IDENTIFIER)
        Id = getName(**IdentItr), Str.append(Id.begin(), Id.end()), ++IdentItr;
      else if (T == TOKEN_UCONST)
        appendUConst(*(UConstItr++), IsSubscript, Str);
      else
        appendToken(T, IsSubscript, Str);
    }
    return true;
}

  /// Unparses the expression and prints result to a specified stream
  /// returns true on success.
  bool print(llvm::raw_ostream &OS) {
    llvm::SmallString<64> Str;
    return toString(Str) ? OS << Str, true : false;
  }

  /// Unparses the expression and prints result to a debug stream,
  /// returns true on success.
  LLVM_DUMP_METHOD bool dump() {
    return print(llvm::dbgs());
  }

private:
  /// \brief Unparses a specified token into a character string.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP. This method should evaluates all tokens except ones that
  /// have a value (constants, identifiers, subscripts).
  /// If `IsSubscript` is true then subscript expression is unparsed. It is
  /// called subsequently for each token except constants between
  /// TOKEN_SUBSCRIPT_BEGIN and TOKEN_SUBSCRIPT_END tokens.
  void appendToken(
      Token T, bool IsSubscript, llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->appendToken(T, IsSubscript, Str);
  }

  /// \brief Unparses a specified unsigned constant into a character string.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  /// If `IsSubscript` is true then subscript expression is unparsed. It is
  /// called subsequently for each constants between TOKEN_SUBSCRIPT_BEGIN and
  /// TOKEN_SUBSCRIPT_END tokens.
  void appendUConst(
      uint64_t C, bool IsSubscript, llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->appendUConst(C, IsSubscript, Str);
  }

  /// \brief Unparses beginning of subscript expressions.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void beginSubscript(llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->beginSubscript(Str);
  }

  /// \brief Unparses ending of subscript expressions.
  ///
  /// This method should be implemented in a child class, it will be called
  /// using CRTP.
  void endSubscript(llvm::SmallVectorImpl<char> &Str) {
    static_cast<Unparser *>(this)->endSubscript(Str);
  }
};
}
#endif//TSAR_SOURCE_UNPARSER_H
