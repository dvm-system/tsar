//===-- tsar_matcher.h ---- High and Low Level Matcher ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
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

#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <set>
#include <transparent_queue.h>
#include "tsar_bimap.h"
#include "tsar_utility.h"

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
    auto TK = getTombstoneKey();
    auto EK = getEmptyKey();
    return LHS == RHS ||
      RHS != TK && LHS != TK && RHS != EK && LHS != EK &&
      LHS->getLine() == RHS->getLine() &&
      LHS->getColumn() == RHS->getColumn() &&
      LHS->getFilename() == RHS->getFilename();
  }
  static bool isEqual(const clang::PresumedLoc &LHS, const DILocation *RHS) {
    return !isEqual(RHS, getTombstoneKey()) &&
      !isEqual(RHS, getEmptyKey()) &&
      LHS.getLine() == RHS->getLine() &&
      LHS.getColumn() == RHS->getColumn() &&
      LHS.getFilename() == RHS->getFilename();
  }
};
}

namespace tsar {
/// This is a base class which is inherited to match different entities (loops,
/// variables, etc.).
///
/// \tparam IRPtrTy Pointer to IR entity.
/// \tparam ASTPtrTy Pointer to AST entity.
template<class IRPtrTy, class ASTPtrTy,
  class IRLocationTy = llvm::DILocation *,
  class IRLocationMapInfo = llvm::DILocationMapInfo,
  class ASTLocationTy = unsigned,
  class ASTLocationMapInfo = llvm::DenseMapInfo<ASTLocationTy>,
  class MatcherTy = Bimap<
    bcl::tagged<ASTPtrTy *, AST>, bcl::tagged<IRPtrTy *, IR>>,
  class UnmatchedASTSetTy = std::set<ASTPtrTy *>>
class MatchASTBase {
public:
  typedef MatcherTy Matcher;

  typedef UnmatchedASTSetTy UnmatchedASTSet;

  /// This is a map from entity location to a queue of IR entities.
  typedef llvm::DenseMap<IRLocationTy,
    bcl::TransparentQueue<IRPtrTy>, IRLocationMapInfo> LocToIRMap;

  /// \brief This is a map from location in a source file to an queue of AST
  /// entities, which are associated with this location.
  ///
  /// The key in this map is a raw encoding for location.
  /// To decode it use SourceLocation::getFromRawEncoding() method.
  typedef llvm::DenseMap<ASTLocationTy,
    bcl::TransparentQueue<ASTPtrTy>, ASTLocationMapInfo> LocToASTMap;

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
  IRPtrTy * findIRForLocation(clang::SourceLocation Loc) {
    auto LocItr = findItrForLocation(Loc);
    if (LocItr == mLocToIR->end())
      return nullptr;
    return LocItr->second.pop();
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
  void matchInMacro(llvm::Statistic &NumMatch, llvm::Statistic &NumNonMatchAST,
      llvm::Statistic &NumNonMatchIR) {
    for (auto &InMacro : *mLocToMacro) {
      clang:: PresumedLoc PLoc = mSrcMgr->getPresumedLoc(
        clang::SourceLocation::getFromRawEncoding(InMacro.first), false);
      auto IREntityItr = mLocToIR->find_as(PLoc);
      // If sizes of queues of AST and IR entities are not equal this is mean
      // that there are implicit entities (for example, implicit loops) in
      // a macro. Such entities are not going to be evaluated due to necessity
      // of additional analysis of AST.
      if (IREntityItr == mLocToIR->end() ||
        IREntityItr->second.size() != InMacro.second.size()) {
        NumNonMatchAST += InMacro.second.size();
        while (auto ASTEntity = InMacro.second.pop())
          mUnmatchedAST->insert(ASTEntity);
      } else {
        NumMatch += InMacro.second.size();
        NumNonMatchIR -= InMacro.second.size();
        while (auto ASTEntity = InMacro.second.pop())
          mMatcher->emplace(ASTEntity, IREntityItr->second.pop());
      }
    }
  }

protected:
  Matcher *mMatcher;
  clang::SourceManager *mSrcMgr;
  LocToIRMap *mLocToIR;
  LocToASTMap *mLocToMacro;
  UnmatchedASTSet *mUnmatchedAST;
};
}
#endif//TSAR_MATCHER_H
