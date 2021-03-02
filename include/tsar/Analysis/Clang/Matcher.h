//===-- Matcher.h --------- High and Low Level Matcher ----------*- C++ -*-===//
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
//
//===----------------------------------------------------------------------===//
//
// This file defines general classes and functions to match some entities
// (loops, variables, etc.) in a source high-level code and appropriate entities
// (loops, allocas, etc.) in low-level LLVM IR.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MATCHER_H
#define TSAR_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/MetadataUtils.h"
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Path.h>
#include <set>

namespace llvm {
/// \brief Implementation of a DenseMapInfo for DILocation *.
///
/// To generate hash value pair of line and column is used. It is possible to
/// use find_as() method with a parameter of type clang::PresumedLoc.
struct DILocationMapInfo {
  static inline DILocation * getEmptyKey() {
    return DenseMapInfo<DILocation *>::getEmptyKey();
  }
  static inline DILocation * getTombstoneKey() {
    return DenseMapInfo<DILocation *>::getTombstoneKey();
  }
  static unsigned getHashValue(const DILocation *Loc) {
    auto Line = Loc->getLine();
    auto Column = Loc->getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static unsigned getHashValue(const clang::PresumedLoc &PLoc) {
    auto Line = PLoc.getLine();
    auto Column = PLoc.getColumn();
    auto Pair = std::make_pair(Line, Column);
    return DenseMapInfo<decltype(Pair)>::getHashValue(Pair);
  }
  static bool isEqual(const DILocation *LHS, const DILocation *RHS) {
    if (LHS == RHS)
      return true;
    auto TK = getTombstoneKey();
    auto EK = getEmptyKey();
    if (RHS == TK || LHS == TK || RHS == EK || LHS == EK ||
        LHS->getLine() != RHS->getLine() ||
        LHS->getColumn() != RHS->getColumn())
      return false;
    sys::fs::UniqueID LHSId, RHSId;
    SmallString<128> LHSPath, RHSPath;
    return !sys::fs::getUniqueID(
               tsar::getAbsolutePath(*LHS->getScope(), LHSPath), LHSId) &&
           !sys::fs::getUniqueID(
               tsar::getAbsolutePath(*RHS->getScope(), RHSPath), RHSId) &&
           LHSId == RHSId;
  }
  static bool isEqual(const clang::PresumedLoc &LHS, const DILocation *RHS) {
    if (isEqual(RHS, getTombstoneKey()) || isEqual(RHS, getEmptyKey()) ||
        LHS.getLine() != RHS->getLine() || LHS.getColumn() != RHS->getColumn())
      return false;
    sys::fs::UniqueID LHSId, RHSId;
    SmallString<128> LHSPath, RHSPath;
    return !sys::fs::getUniqueID(LHS.getFilename(), LHSId) &&
           !sys::fs::getUniqueID(
               tsar::getAbsolutePath(*RHS->getScope(), RHSPath), RHSId) &&
           LHSId == RHSId;
  }
};
}

namespace tsar {
/// This is a base class which is inherited to match different entities (loops,
/// variables, etc.).
template<class IRItemTy, class ASTItemTy,
  class IRLocationTy = llvm::DILocation *,
  class IRLocationMapInfo = llvm::DILocationMapInfo,
  class ASTLocationTy = unsigned,
  class ASTLocationMapInfo = llvm::DenseMapInfo<ASTLocationTy>,
  class MatcherTy = Bimap<
    bcl::tagged<ASTItemTy, AST>, bcl::tagged<IRItemTy, IR>>,
  class UnmatchedASTSetTy = llvm::DenseSet<ASTItemTy>>
class MatchASTBase {
public:
  using Matcher = MatcherTy;

  using UnmatchedASTSet = UnmatchedASTSetTy;

  /// This is a map from entity location to a queue of IR entities.
  using LocToIRMap =
      llvm::DenseMap<IRLocationTy,
                     std::conditional_t<std::is_pointer_v<IRItemTy>,
                                        llvm::TinyPtrVector<IRItemTy>,
                                        llvm::SmallVector<IRItemTy, 1>>,
                     IRLocationMapInfo>;

  /// \brief This is a map from location in a source file to an queue of AST
  /// entities, which are associated with this location.
  ///
  /// The key in this map is a raw encoding for location.
  /// To decode it use SourceLocation::getFromRawEncoding() method.
  using LocToASTMap =
      llvm::DenseMap<ASTLocationTy,
                     std::conditional_t<std::is_pointer_v<ASTItemTy>,
                                        llvm::TinyPtrVector<ASTItemTy>,
                                        llvm::SmallVector<ASTItemTy, 1>>,
                     ASTLocationMapInfo>;

  /// \brief Constructor.
  ///
  /// \param[in] SrcMgr Clang source manager to deal with locations.
  /// \param[in, out] M Representation of match.
  /// \param[in, out] UM Storage for unmatched ast entities.
  /// \param[in, out] LocToIR Map from entity location to a queue
  /// of IR entities.
  /// \param[in, out] LocToMacro A map form entity expansion location to a queue
  /// of AST entities. All entities explicitly (not implicit loops) defined in
  /// macros is going to store in this map. The key in this map is a raw
  /// encoding for expansion location.
  MatchASTBase(clang::SourceManager &SrcMgr, Matcher &M, UnmatchedASTSet &UM,
    LocToIRMap &LocToIR, LocToASTMap &LocToMacro) :
    mSrcMgr(&SrcMgr), mMatcher(&M), mUnmatchedAST(&UM),
    mLocToIR(&LocToIR), mLocToMacro(&LocToMacro) {}

  /// Finds low-level representation of an entity at the specified location.
  ///
  /// \return LLVM IR for an entity or `nullptr`.
  IRItemTy findIRForLocation(clang::SourceLocation Loc) {
    auto LocItr = findItrForLocation(Loc);
    if (LocItr == mLocToIR->end())
      return nullptr;
    auto Res = LocItr->second.back();
    LocItr->second.pop_back();
    return Res;
  }

  /// Finds low-level representation of an entity at the specified location.
  ///
  /// \return Iterator to an element in LocToIRMap.
  typename LocToIRMap::iterator findItrForLocation(clang::SourceLocation Loc) {
    if (Loc.isInvalid())
      return mLocToIR->end();
    auto PLoc = mSrcMgr->getPresumedLoc(Loc, false);
    return mLocToIR->find_as(PLoc);
  }

  /// Evaluates entities located in macros.
  ///
  /// This matches entities from mLocToMacro and mLocToIR. It is recommended
  /// to evaluate at first all entities outside macros and then consider macros
  /// separately.
  ///
  /// \param [in] Strict If it is true, macros, containing exactly a single
  /// item, are processed.
  void matchInMacro(llvm::Statistic &NumMatch, llvm::Statistic &NumNonMatchAST,
      llvm::Statistic &NumNonMatchIR, bool Strict = false) {
    for (auto &InMacro : *mLocToMacro) {
      clang:: PresumedLoc PLoc = mSrcMgr->getPresumedLoc(
        clang::SourceLocation::getFromRawEncoding(InMacro.first), false);
      auto IREntityItr = mLocToIR->find_as(PLoc);
      // If sizes of queues of AST and IR entities are not equal this is mean
      // that there are implicit entities (for example, implicit loops) in
      // a macro. Such entities are not going to be evaluated due to necessity
      // of additional analysis of AST.
      if (IREntityItr == mLocToIR->end() ||
          IREntityItr->second.size() != InMacro.second.size() ||
          Strict && InMacro.second.size() > 1) {
        NumNonMatchAST += InMacro.second.size();
        while (!InMacro.second.empty()) {
          mUnmatchedAST->insert(InMacro.second.back());
          InMacro.second.pop_back();
        }
      } else {
        NumMatch += InMacro.second.size();
        NumNonMatchIR -= InMacro.second.size();
        while (!InMacro.second.empty()) {
          mMatcher->emplace(InMacro.second.back(), IREntityItr->second.back());
          InMacro.second.pop_back();
          IREntityItr->second.pop_back();
        }
      }
    }
  }

protected:
  clang::SourceManager *mSrcMgr;
  Matcher *mMatcher;
  UnmatchedASTSet *mUnmatchedAST;
  LocToIRMap *mLocToIR;
  LocToASTMap *mLocToMacro;
};
}
#endif//TSAR_MATCHER_H
