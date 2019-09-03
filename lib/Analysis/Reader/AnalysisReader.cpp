//===- AnalysisReader.cpp -- Reader For DYNA Results -------------*- C++ -*===//
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
// This file implements a pass to load results of analysis and to integrate
// these results with TSAR structures. For example, this pass can be used to
// access results of dynamic analysis.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Reader/AnalysisJSON.h"
#include "tsar/Analysis/Reader/Passes.h"
#include <bcl/cell.h>
#include <bcl/utility.h>
#include <bcl/tagged.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/Pass.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Debug.h>
#include <map>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "analysis-reader"

namespace {
struct File {};
struct Line {};
struct Column {};
struct Identifier {};

/// Position in a source code.
using LocationT = bcl::tagged_tuple<
  bcl::tagged<std::string, File>,
  bcl::tagged<trait::LineTy, Line>,
  bcl::tagged<trait::ColumnTy, Column>>;

/// Map from a loop location to a loop index in a list of loops in
/// external analysis results.
using LoopCache = std::map<LocationT, std::size_t>;

/// Position of a variable in a source code.
using VariableT = bcl::tagged_tuple<
  bcl::tagged<std::string, File>,
  bcl::tagged<trait::LineTy, Line>,
  bcl::tagged<trait::ColumnTy, Column>,
  bcl::tagged<std::string, Identifier>>;

/// Tuple of iterators of variable traits stored in external analysis results.
using TraitT = bcl::tagged_tuple<
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Private::ValueType::const_iterator>,
      trait::Private>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::UseAfterLoop::ValueType::const_iterator>,
      trait::UseAfterLoop>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Anti::ValueType::const_iterator>,
      trait::Anti>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Flow::ValueType::const_iterator>,
      trait::Flow>
>;

template<std::size_t Idx, class... Tags> struct IsOnlyImpl {
  static bool is(const TraitT &Traits) {
    if (bcl::is_contained<typename bcl::tagged_tuple_tag<Idx, TraitT>::type,
                          Tags...>::value)
      return std::get<Idx>(Traits) && IsOnlyImpl<Idx + 1, Tags...>::is(Traits);
    else
      return !std::get<Idx>(Traits) && IsOnlyImpl<Idx + 1, Tags...>::is(Traits);
  }
};

template<class... Tags>
struct IsOnlyImpl<bcl::tagged_tuple_size<TraitT>::value, Tags...> {
  static bool is(const TraitT &) noexcept { return true; }
};

/// Return true if only specified traits are set.
template<class... Tags> bool isOnly(const TraitT &Traits) {
  return IsOnlyImpl<0, Tags...>::is(Traits);
}

/// Map from variable to its traits in some loop.
using TraitCache = std::map<VariableT, TraitT>;

/// This pass load results from a specified file and update traits of
/// metadata-level memory locations accessed in loops.
class AnalysisReader : public FunctionPass, bcl::Uncopyable {
public:
  static char ID;

  AnalysisReader() : FunctionPass(ID) {
    initializeAnalysisReaderPass(*PassRegistry::getPassRegistry());
  }

  explicit AnalysisReader(StringRef DataFile) :
    mDataFile(DataFile), FunctionPass(ID) {
    initializeAnalysisReaderPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::string mDataFile;
};

/// Extract a list of analyzed loops from external analysis results.
LoopCache buildLoopCache(const trait::Info &Info) {
  LoopCache Res;
  for (std::size_t I = 0, EI = Info[trait::Info::Loops].size(); I < EI; ++I) {
    auto &L = Info[trait::Info::Loops][I];
    LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: add loop to cache "
                      << L[trait::Loop::File] << ":" << L[trait::Loop::Line]
                      << ":" << L[trait::Loop::Column] << "\n");
    Res.emplace(LocationT{
      L[trait::Loop::File], L[trait::Loop::Line], L[trait::Loop::Column]}, I);
  }
  return Res;
}

/// Extract a list of traits for a specified loop `L` from external analysis
/// results.
TraitCache buildTraitCache(const trait::Info &Info, const trait::Loop &L) {
  auto createVar = [&Info](std::size_t I) {
    VariableT Var;
    Var.get<File>() = Info[trait::Info::Vars][I][trait::Var::File];
    Var.get<Line>() = Info[trait::Info::Vars][I][trait::Var::Line];
    Var.get<Column>() = Info[trait::Info::Vars][I][trait::Var::Column];
    Var.get<Identifier>() = Info[trait::Info::Vars][I][trait::Var::Name];
    return Var;
  };
  TraitCache Res;
  for (auto I = L[trait::Loop::Private].cbegin(),
       EI = L[trait::Loop::Private].cend(); I != EI; ++I) {
    auto Pair = Res.emplace(createVar(*I), TraitT());
    Pair.first->second.get<trait::Private>() = I;
  }
  for (auto I = L[trait::Loop::UseAfterLoop].cbegin(),
       EI = L[trait::Loop::UseAfterLoop].cend(); I != EI; ++I) {
    auto Var = createVar(*I);
    auto ResItr = Res.find(Var);
    if (ResItr == Res.end())
      ResItr = Res.emplace(std::move(Var), TraitT()).first;
    ResItr->second.get<trait::UseAfterLoop>() = I;
  }
  for (auto I = L[trait::Loop::Anti].cbegin(),
       EI = L[trait::Loop::Anti].cend(); I != EI; ++I) {
    auto Var = createVar(I->first);
    auto ResItr = Res.find(Var);
    if (ResItr == Res.end())
      ResItr = Res.emplace(std::move(Var), TraitT()).first;
    ResItr->second.get<trait::Anti>() = I;
  }
  for (auto I = L[trait::Loop::Flow].cbegin(),
       EI = L[trait::Loop::Flow].cend(); I != EI; ++I) {
    auto Var = createVar(I->first);
    auto ResItr = Res.find(Var);
    if (ResItr == Res.end())
      ResItr = Res.emplace(std::move(Var), TraitT()).first;
    ResItr->second.get<trait::Flow>() = I;
  }
  return Res;
}

/// Find traits for a specified loop in external analysis results.
const trait::Loop * findLoop(const MDNode *LoopID, const LoopCache &Cache,
    const trait::Info &Info) {
  DILocation *Loc = nullptr;
  for (unsigned I = 1, EI = LoopID->getNumOperands(); I < EI; ++I)
    if (Loc = dyn_cast<DILocation>(LoopID->getOperand(I)))
      break;
  if (!Loc)
    return nullptr;
  auto LoopKey =
    LocationT{ Loc->getFilename().str(), Loc->getLine(), Loc->getColumn() };
  auto LoopItr = Cache.find(LoopKey);
  return (LoopItr == Cache.end() ? nullptr :
    &Info[trait::Info::Loops][LoopItr->second]);
}

/// Update description `DITrait` of a specified trait `TraitTag` according to
/// external information `TraitItr`.
template<class TraitTag> void updateDependence(
    const TraitCache::iterator &TraitItr, DIMemoryTrait &DITrait) {
  static_assert(std::is_same<TraitTag, trait::Flow>::value ||
    std::is_same<TraitTag, trait::Anti>::value ||
    std::is_same<TraitTag, trait::Output>::value, "Unknown type of dependence!");
  if (!TraitItr->second.template get<TraitTag>() ||
      !DITrait.template is<TraitTag>())
    return;
  LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update " << TraitTag::toString()
                    << " dependence\n");
  auto F = trait::Dependence::Flag::No;
  auto Dep = DITrait.get<TraitTag>();
  if (F)
    F = Dep->getFlags();
  F &= (~trait::Dependence::Flag::UnknownDistance);
  F &= (~trait::Dependence::Flag::May);
  trait::DIDependence::DistanceRange Range;
  auto &T = *TraitItr->second.template get<TraitTag>();
  auto Min = T->second[trait::Distance::Min];
  Range.first = APSInt(APInt(CHAR_BIT * sizeof(Min), Min, true), false);
  auto Max = T->second[trait::Distance::Max];
  Range.second = APSInt(APInt(CHAR_BIT * sizeof(Max), Max, true), false);
  DITrait.template set<TraitTag>(new trait::DIDependence(F, std::move(Range)));
  LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: set distance to [" << Min << ", "
                    << Max << "]\n");
}
}

INITIALIZE_PASS_BEGIN(AnalysisReader, "analysis-reader",
  "External Analysis Results Reader", true, true)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_END(AnalysisReader, "analysis-reader",
  "External Analysis Results Reader", true, true)

char AnalysisReader::ID = 0;

FunctionPass * llvm::createAnalysisReader(StringRef DataFile) {
  return new AnalysisReader(DataFile);
}

void AnalysisReader::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DIMemoryTraitPoolWrapper>();
}

bool AnalysisReader::runOnFunction(Function &F) {
  auto FileOrErr = MemoryBuffer::getFile(mDataFile);
  if (auto EC = FileOrErr.getError()) {
    F.getContext().diagnose(DiagnosticInfoPGOProfile(mDataFile.data(),
      Twine("unable to open file: ") + EC.message()));
    return false;
  }
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  json::Parser<> Parser((**FileOrErr).getBuffer().str());
  trait::Info Info;
  if (!Parser.parse(Info)) {
    for (auto D : Parser.errors()) {
      DiagnosticInfoPGOProfile Diag(mDataFile.data(), D, DS_Note);
      F.getContext().diagnose(Diag);
    }
    F.getContext().diagnose(DiagnosticInfoPGOProfile(mDataFile.data(),
      "unable to parse external analysis results"));
    return false;
  }
  auto LoopCache = buildLoopCache(Info);
  for (auto &TraitLoop : TraitPool) {
    auto LoopID = cast<MDNode>(TraitLoop.get<Region>());
    auto *L = findLoop(LoopID, LoopCache, Info);
    if (!L)
      continue;
    LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update traits for loop at "
                      << (*L)[trait::Loop::File] << ":"
                      << (*L)[trait::Loop::Line] << ":"
                      << (*L)[trait::Loop::Column] << "\n");
    auto TraitCache = buildTraitCache(Info, *L);
    for (auto &DITrait : *TraitLoop.get<Pool>()) {
      if (DITrait.is_any<trait::NoAccess, trait::Readonly, trait::Reduction,
                         trait::Induction>())
        continue;
      auto *DIEM = dyn_cast<DIEstimateMemory>(DITrait.getMemory());
      if (!DIEM)
        continue;
      auto *DIVar = DIEM->getVariable();
      auto *DIExpr = DIEM->getExpression();
      assert(DIVar && DIExpr && "Invalid memory location!");
      if (DIExpr->getNumElements() > 1)
        continue;
      if (DIExpr->getNumElements() == 1 && !DIExpr->startsWithDeref())
        continue;
      VariableT Var;
      SmallVector<DebugLoc, 1> DbgLocs;
      DIEM->getDebugLoc(DbgLocs);
      if (DbgLocs.size() > 1)
        continue;
      if (DbgLocs.empty()) {
        // Column may be omitted for global variables only, because there are
        // no more than one variable with the same name in a global scope.
        if (!isa<DIGlobalVariable>(DIVar))
          continue;
        Var.get<Line>() = DIVar->getLine();
        Var.get<Column>() = 0;
      } else {
        Var.get<Line>() = DbgLocs.front().getLine();
        Var.get<Column>() = DbgLocs.front().getCol();
      }
      Var.get<Identifier>() =
        ((DIExpr->startsWithDeref() ? "^" : "") + DIVar->getName()).str();
      Var.get<File>() = DIVar->getFilename();
      LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update traits for a variable "
                        << Var.get<Identifier>() << " defined at "
                        << Var.get<File>() << ":" << Var.get<Line>() << ":"
                        << Var.get<Column>() << "\n");
      auto TraitItr = TraitCache.find(Var);
      if (TraitItr == TraitCache.end() ||
          isOnly<trait::UseAfterLoop>(TraitItr->second)) {
        // It may not be readonly. In the following example `A` is
        // not privatizable, however there is no dependence in this loop.
        // for (int I = 0; I < N; I += 2)
        //   A[I] = A[I + 1];
        // TODO (kaniandr@gmail.com): add readonly property to external
        // analysis results.
        DITrait.set<trait::Shared>();
      } else if (TraitItr->second.get<trait::Private>()) {
        if (!TraitItr->second.get<trait::UseAfterLoop>())
          DITrait.set<trait::Private>();
        else if (DITrait.is_any<trait::Flow, trait::Anti, trait::Output>())
          DITrait.set<trait::DynamicPrivate>();
        // TODO (kaniandr@gmail.com): it is not possible to set shared property
        // here, because there is no 'output' property in external analysis
        // results. In the following example, there is 'output' dependence and
        // there is no any information about A in external analysis results.
        // for (int I = 0; I < N; ++I)
        //   A[5] = I;
#if 0
        // Set 'shared' property after if-stmt, because 'shared' property unset
        // anti/flow/output properties.
        if (!TraitItr->second.get<Flow>() &&
            !TraitItr->second.get<Anti>() &&
            !TriatItr->second.get<Output>()))
          DITrait.set<Shared>();
#endif
      } else if (DITrait.is<trait::FirstPrivate>()) {
        if (!TraitItr->second.get<trait::UseAfterLoop>())
          DITrait.unset<trait::LastPrivate, trait::SecondToLastPrivate,
                        trait::DynamicPrivate>();

      } else {
        updateDependence<trait::Anti>(TraitItr, DITrait);
        updateDependence<trait::Flow>(TraitItr, DITrait);
        if (!TraitItr->second.get<trait::Flow>())
          DITrait.unset<trait::Flow>();
        if (!TraitItr->second.get<trait::Anti>())
          DITrait.unset<trait::Anti>();
      }
      LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: set traits to ";
                 DITrait.print(dbgs()); dbgs() << "\n");
    }
  }
  return false;
}
