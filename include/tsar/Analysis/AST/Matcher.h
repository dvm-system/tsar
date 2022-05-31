//===-- Matcher.h --------- High and Low Level Matcher ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
#include "tsar/Support/DILocationMapInfo.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/MetadataUtils.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace tsar {
/// This is a base class which is inherited to match different entities (loops,
/// variables, etc.).
///
/// The ImplTy class must inherit this class and implement following methods:
/// - typename LocToIRMap::iterator findItrForLocation(ASTLocationTy Loc);
/// - PresumedLocationTy getPresumedLocation(RawLocationTy Loc);
/// Note, that tsar::PresumedLocationInfo<...> template must be specialized for
/// the PresumedLocationTy type.
template<typename ImplTy, typename IRItemTy, typename ASTItemTy,
  typename ASTLocationTy,
  typename IRLocationTy = llvm::DILocation *,
  typename IRLocationMapInfo = DILocationMapInfo,
  typename RawLocationTy = std::size_t,
  typename RawLocationMapInfo = llvm::DenseMapInfo<RawLocationTy>,
  typename MatcherTy = Bimap<
    bcl::tagged<ASTItemTy, AST>, bcl::tagged<IRItemTy, IR>>,
  typename UnmatchedASTSetTy = llvm::DenseSet<ASTItemTy>>
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

  /// This is a map from location in a source file to an queue of AST
  /// entities, which are associated with this location.
  ///
  /// The key in this map is a raw encoding for location.
  using LocToASTMap =
      llvm::DenseMap<RawLocationTy,
                     std::conditional_t<std::is_pointer_v<ASTItemTy>,
                                        llvm::TinyPtrVector<ASTItemTy>,
                                        llvm::SmallVector<ASTItemTy, 1>>,
                     RawLocationMapInfo>;

  /// \brief Constructor.
  ///
  /// \param[in, out] M Representation of match.
  /// \param[in, out] UM Storage for unmatched ast entities.
  /// \param[in, out] LocToIR Map from entity location to a queue
  /// of IR entities.
  /// \param[in, out] LocToMacro A map form entity expansion location to a queue
  /// of AST entities. All entities explicitly (not implicit loops) defined in
  /// macros is going to store in this map. The key in this map is a raw
  /// encoding for expansion location.
  MatchASTBase(Matcher &M, UnmatchedASTSet &UM,
    LocToIRMap &LocToIR, LocToASTMap &LocToMacro) :
    mMatcher(&M), mUnmatchedAST(&UM),
    mLocToIR(&LocToIR), mLocToMacro(&LocToMacro) {}

  /// Finds low-level representation of an entity at the specified location.
  ///
  /// \return LLVM IR for an entity or `nullptr`.
  IRItemTy findIRForLocation(ASTLocationTy Loc) {
    auto LocItr = static_cast<ImplTy *>(this)->findItrForLocation(Loc);
    if (LocItr == mLocToIR->end())
      return nullptr;
    auto Res = LocItr->second.back();
    LocItr->second.pop_back();
    return Res;
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
      auto PLoc{static_cast<ImplTy *>(this)->getPresumedLoc(InMacro.first)};
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
  Matcher *mMatcher;
  UnmatchedASTSet *mUnmatchedAST;
  LocToIRMap *mLocToIR;
  LocToASTMap *mLocToMacro;
};
}
#endif//TSAR_MATCHER_H
