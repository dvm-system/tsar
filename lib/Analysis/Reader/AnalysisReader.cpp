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
#include "tsar/Analysis/Memory/MemoryTraitJSON.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Reader/AnalysisJSON.h"
#include "tsar/Analysis/Reader/Passes.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Unparse/VariableLocation.h"
#include <bcl/cell.h>
#include <bcl/utility.h>
#include <bcl/tagged.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/Pass.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Debug.h>
#include <map>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "analysis-reader"

namespace {
/// Position of a function in a source code.
using FunctionT = bcl::tagged_tuple<
  bcl::tagged<sys::fs::UniqueID, File>,
  bcl::tagged<trait::LineTy, Line>,
  bcl::tagged<std::string, Identifier>>;

/// Map from a function location to a function index in the list of functions in
/// external analysis results.
using FunctionCache = std::map<FunctionT, std::size_t>;

/// Position in a source code.
using LocationT = bcl::tagged_tuple<
  bcl::tagged<sys::fs::UniqueID, File>,
  bcl::tagged<trait::LineTy, Line>,
  bcl::tagged<trait::ColumnTy, Column>>;

/// Map from a loop location to a loop index in a list of loops in
/// external analysis results.
using LoopCache = std::map<LocationT, std::size_t>;

/// Tuple of iterators of variable traits stored in external analysis results.
using TraitT = bcl::tagged_tuple<
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Reduction::ValueType::const_iterator>,
      trait::Reduction>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Private::ValueType::const_iterator>,
      trait::Private>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::UseAfterLoop::ValueType::const_iterator>,
      trait::UseAfterLoop>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::WriteOccurred::ValueType::const_iterator>,
      trait::WriteOccurred>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::ReadOccurred::ValueType::const_iterator>,
      trait::ReadOccurred>,
  bcl::tagged<
    Optional<trait::json_::LoopImpl::Output::ValueType::const_iterator>,
      trait::Output>,
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

template<std::size_t Idx, class... Tags> struct IsOnlyAnyOfImpl {
  static bool is(const TraitT &Traits) {
    // We have to only check that traits except Tags... are not set.
    if (bcl::is_contained<typename bcl::tagged_tuple_tag<Idx, TraitT>::type,
                          Tags...>::value)
      return IsOnlyAnyOfImpl<Idx + 1, Tags...>::is(Traits);
    return !std::get<Idx>(Traits) &&
      IsOnlyAnyOfImpl<Idx + 1, Tags...>::is(Traits);
  }
};

template<class... Tags>
struct IsOnlyAnyOfImpl<bcl::tagged_tuple_size<TraitT>::value, Tags...> {
  static bool is(const TraitT &) noexcept { return true; }
};

/// Return true if only specified traits are set.
template<class... Tags> bool isOnly(const TraitT &Traits) {
  return IsOnlyImpl<0, Tags...>::is(Traits);
}

/// Return true if only some of specified traits are set or
/// specified traits is not set at all.
template<class... Tags> bool isOnlyAnyOf(const TraitT &Traits) {
  return IsOnlyAnyOfImpl<0, Tags...>::is(Traits);
}

/// Map from variable to its traits in some loop.
using TraitCache = std::map<VariableLocationT, TraitT>;

/// This pass load results from a specified file and update traits of
/// metadata-level memory locations accessed in loops.
class AnalysisReader : public FunctionPass, bcl::Uncopyable {
public:
  static char ID;

  explicit AnalysisReader(): FunctionPass(ID) {
    initializeAnalysisReaderPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  bool load(Function &F, unsigned DWLang, StringRef DataFile);
};

/// Extract a list of analyzed functions from external analysis results.
FunctionCache buildFunctionCache(const trait::Info &Info) {
    FunctionCache Res;
  for (std::size_t I = 0, EI = Info[trait::Info::Functions].size(); I < EI;
       ++I) {
    auto &F = Info[trait::Info::Functions][I];
    LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: add function to cache "
                      << F[trait::Function::Name] << ":"
                      << F[trait::Function::File] << ":"
                      << F[trait::Function::Line] << ":"
                      << F[trait::Function::Column] << "\n");
    sys::fs::UniqueID ID;
    if (sys::fs::getUniqueID(F[trait::Function::File], ID))
      continue;
    Res.emplace(
        FunctionT{ID, F[trait::Function::Line], F[trait::Function::Name]}, I);
  }
  return Res;
}

/// Extract a list of analyzed loops from external analysis results.
LoopCache buildLoopCache(const trait::Info &Info) {
  LoopCache Res;
  for (std::size_t I = 0, EI = Info[trait::Info::Loops].size(); I < EI; ++I) {
    auto &L = Info[trait::Info::Loops][I];
    LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: add loop to cache "
      << L[trait::Loop::File] << ":" << L[trait::Loop::Line]
      << ":" << L[trait::Loop::Column] << "\n");
    sys::fs::UniqueID ID;
    if (sys::fs::getUniqueID(L[trait::Loop::File], ID))
      continue;
    Res.emplace(LocationT{ID, L[trait::Loop::Line], L[trait::Loop::Column]}, I);
  }
  return Res;
}

VariableLocationT createVar(trait::IdTy I, const trait::Info &Info) {
  /// TODO (kaniandr@gmail.com): check that index of variable is not out of
  /// range due to incorrect .json created manually.
  VariableLocationT Var;
  sys::fs::UniqueID ID;
  if (sys::fs::getUniqueID(Info[trait::Info::Vars][I][trait::Var::File], ID)) {
    LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: ignore variable "
                      << Info[trait::Info::Vars][I][trait::Var::Name]
                      << ", unable to build unique ID for a file "
                      << Info[trait::Info::Vars][I][trait::Var::File] << "\n");
    return Var;
  }
  Var.get<File>() = ID;
  Var.get<Line>() = Info[trait::Info::Vars][I][trait::Var::Line];
  Var.get<Column>() = Info[trait::Info::Vars][I][trait::Var::Column];
  Var.get<Identifier>() = Info[trait::Info::Vars][I][trait::Var::Name];
  return Var;
}

trait::IdTy getVariableIdx(
    std::set<trait::IdTy>::const_iterator I) {
  return *I;
}

trait::IdTy getVariableIdx(
    std::map<trait::IdTy, trait::Distance>::const_iterator I) {
  return I->first;
}

trait::IdTy getVariableIdx(
    std::map<trait::IdTy, trait::Reduction::Kind>::const_iterator I) {
  return I->first;
}

template<class Tag, class ExternalTag> void addToCache(ExternalTag Key,
    const trait::Info &Info, const trait::Loop &L, TraitCache &Cache) {
  for (auto I = L[Key].cbegin(), EI = L[Key].cend(); I != EI; ++I) {
    auto Var = createVar(getVariableIdx(I), Info);
    if (Var.template get<Identifier>().empty())
      continue;
    auto CacheItr = Cache.find(Var);
    if (CacheItr == Cache.end())
      CacheItr = Cache.emplace(std::move(Var), TraitT()).first;
    CacheItr->second.template get<Tag>() = I;
  }
}

/// Extract a list of traits for a specified loop `L` from external analysis
/// results.
TraitCache buildTraitCache(const trait::Info &Info, const trait::Loop &L) {
  TraitCache Res;
  addToCache<trait::Reduction>(trait::Loop::Reduction, Info, L, Res);
  addToCache<trait::Private>(trait::Loop::Private, Info, L, Res);
  addToCache<trait::UseAfterLoop>(trait::Loop::UseAfterLoop, Info, L, Res);
  addToCache<trait::WriteOccurred>(trait::Loop::WriteOccurred, Info, L, Res);
  addToCache<trait::ReadOccurred>(trait::Loop::ReadOccurred, Info, L, Res);
  addToCache<trait::Output>(trait::Loop::Output, Info, L, Res);
  addToCache<trait::Anti>(trait::Loop::Anti, Info, L, Res);
  addToCache<trait::Flow>(trait::Loop::Flow, Info, L, Res);
  return Res;
}

const trait::Function *findFunction(const DISubprogram *DISub,
    const FunctionCache &Cache, const trait::Info &Info) {
  SmallString<128> Path;
  sys::fs::UniqueID ID;
  if (sys::fs::getUniqueID(getAbsolutePath(*DISub, Path), ID))
    return nullptr;
  FunctionT F{ID, DISub->getLine(), DISub->getName()};
  auto FuncItr = Cache.find(F);
  if (FuncItr != Cache.end())
    return &Info[trait::Info::Functions][FuncItr->second];
  F.get<Identifier>() = DISub->getLinkageName().str();
  FuncItr = Cache.find(F);
  return (FuncItr == Cache.end())
             ? nullptr
             : &Info[trait::Info::Functions][FuncItr->second];
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
  SmallString<128> Path;
  sys::fs::UniqueID ID;
  if (sys::fs::getUniqueID(getAbsolutePath(*Loc->getScope(), Path), ID))
    return nullptr;
  auto LoopKey =
    LocationT{ ID, Loc->getLine(), Loc->getColumn() };
  auto LoopItr = Cache.find(LoopKey);
  return (LoopItr == Cache.end() ? nullptr :
    &Info[trait::Info::Loops][LoopItr->second]);
}

/// Update description `DITrait` of a specified trait `TraitTag` according to
/// external information `TraitItr`.
template<class TraitTag> void updateAntiFlowDep(
    const TraitCache::iterator &TraitItr, DIMemoryTrait &DITrait) {
  static_assert(std::is_same<TraitTag, trait::Flow>::value ||
    std::is_same<TraitTag, trait::Anti>::value, "Unknown type of dependence!");
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
  trait::DIDependence::DistanceVector DistVector(
      Dep->getLevels() > 0 ? Dep->getLevels() : 1);
  auto &T = *TraitItr->second.template get<TraitTag>();
  auto Min = T->second[trait::Distance::Min];
  if (Min)
    DistVector[0].first =
        APSInt(APInt(CHAR_BIT * sizeof(*Min), *Min, true), false);
  auto Max = T->second[trait::Distance::Max];
  if (Max)
    DistVector[0].second =
        APSInt(APInt(CHAR_BIT * sizeof(*Max), *Max, true), false);
  for (unsigned I = 1, EI = Dep->getLevels(); I < EI; ++I)
    DistVector[I] = Dep->getDistance(I);
  DITrait.template set<TraitTag>(new trait::DIDependence(F, DistVector));
  LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: set distance to [";
             if (Min) dbgs() << *Min; else dbgs() << "?"; dbgs() << ", ";
             if (Max) dbgs() << *Max; else dbgs() << "?"; dbgs() << "]\n");
}

/// Update description of output dependence in `DITrait` according to
/// external information `TraitItr`.
void updateOutputDep(
  const TraitCache::iterator &TraitItr, DIMemoryTrait &DITrait) {
  if (!TraitItr->second.template get<trait::Output>() ||
    !DITrait.template is<trait::Output>())
    return;
  LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update " << trait::Output::toString()
    << " dependence\n");
  auto F = trait::Dependence::Flag::No;
  auto Dep = DITrait.get<trait::Output>();
  if (F)
    F = Dep->getFlags();
  F &= (~trait::Dependence::Flag::May);
  trait::DIDependence::DistanceVector DistVector(Dep->getLevels());
  for (unsigned I = 0, EI = Dep->getLevels(); I < EI; ++I)
    DistVector[I] = Dep->getDistance(I);
  DITrait.template set<trait::Output>(new trait::DIDependence(F, DistVector));
}
}

INITIALIZE_PASS_BEGIN(AnalysisReader, "analysis-reader",
  "External Analysis Results Reader", true, true)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_END(AnalysisReader, "analysis-reader",
  "External Analysis Results Reader", true, true)

char AnalysisReader::ID = 0;

FunctionPass * llvm::createAnalysisReader() { return new AnalysisReader; }

void AnalysisReader::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
}

bool AnalysisReader::runOnFunction(Function &F) {
  auto DWLang = getLanguage(F);
  if (!DWLang)
    return false;
  auto &GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  for (auto &File : GO.AnalysisUse)
    load(F, *DWLang, File);
  return false;
}

bool AnalysisReader::load(Function &F, unsigned DWLang, StringRef DataFile) {
  LLVM_DEBUG(
      dbgs() << "[ANALYSIS READER]: load external analysis results from '"
             << DataFile << "'\n");
  auto FileOrErr = MemoryBuffer::getFile(DataFile);
  if (auto EC = FileOrErr.getError()) {
    F.getContext().diagnose(DiagnosticInfoPGOProfile(DataFile.data(),
      Twine("unable to open file: ") + EC.message()));
    return false;
  }
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  json::Parser<> Parser((**FileOrErr).getBuffer().str());
  trait::Info Info;
  if (!Parser.parse(Info)) {
    for (auto D : Parser.errors()) {
      // Build diagnostic in-place of call to diagnose(), because diagnostic
      // class uses temporary objects available only at construction time.
      F.getContext().diagnose(
          DiagnosticInfoPGOProfile(DataFile.data(), D, DS_Note));
    }
    F.getContext().diagnose(DiagnosticInfoPGOProfile(DataFile.data(),
      "unable to parse external analysis results"));
    return false;
  }
  auto FunctionCache = buildFunctionCache(Info);
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
      if (auto *DIUM{ dyn_cast<DIUnknownMemory>(DITrait.getMemory()) };
          DIUM && DIUM->isExec()) {
        auto *MD{DIUM->getMetadata()};
        assert(MD && "MDNode must not be null!");
        if (auto *DISub{dyn_cast<DISubprogram>(MD)})
          if (auto *F{findFunction(DISub, FunctionCache, Info)}) {
            LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update traits for the "
                              << (*F)[trait::Function::Name] << " function at "
                              << (*F)[trait::Function::File] << ":"
                              << (*F)[trait::Function::Line] << ":"
                              << (*F)[trait::Function::Column] << "\n");
            if ((*F)[trait::Function::Pure]) {
              DITrait.unset<trait::DirectAccess, trait::ExplicitAccess,
                            trait::AddressAccess>();
              DITrait.set<trait::Redundant, trait::NoAccess>();
            }
            LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: set traits to ";
                       DITrait.print(dbgs()); dbgs() << "\n");
          }
        continue;
      }
      if (DITrait.is_any<trait::NoAccess, trait::Readonly, trait::Reduction,
                         trait::Induction>())
        continue;
      auto *DIEM = dyn_cast<DIEstimateMemory>(DITrait.getMemory());
      if (!DIEM)
        continue;
      auto *DIVar = DIEM->getVariable();
      if (isStubVariable(*DIVar))
        continue;
      auto *DIExpr = DIEM->getExpression();
      assert(DIVar && DIExpr && "Invalid memory location!");
      auto Var{buildVariable(DWLang, *DIEM)};
      if (!Var)
        continue;
      LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: update traits for a variable "
                        << Var->get<Identifier>() << " defined at "
                        << DIVar->getFilename() << ":" << Var->get<Line>()
                        << ":" << Var->get<Column>() << "\n");
      auto TraitItr = TraitCache.find(*Var);
      if (TraitItr == TraitCache.end()) {
        LLVM_DEBUG(
            dbgs() << "[ANALYSIS READER]: no external traits are provided\n");
        continue;
      }
      if (isOnlyAnyOf<trait::UseAfterLoop, trait::WriteOccurred,
                      trait::ReadOccurred>(TraitItr->second)) {
        if (TraitItr->second.get<trait::WriteOccurred>())
          DITrait.set<trait::Shared>();
        else
          DITrait.set<trait::Readonly>();
      } else if (TraitItr->second.get<trait::Private>()) {
        if (!TraitItr->second.get<trait::UseAfterLoop>())
          DITrait.set<trait::Private>();
        else if (DITrait.is_any<trait::Flow, trait::Anti, trait::Output>())
          DITrait.set<trait::DynamicPrivate>();
        // Set 'shared' property after if-stmt, because 'shared' property unset
        // anti/flow/output properties.
        if (!TraitItr->second.get<trait::Flow>() &&
            !TraitItr->second.get<trait::Anti>() &&
            !TraitItr->second.get<trait::Output>())
          DITrait.set<trait::Shared>();
      } else if (DITrait.is<trait::FirstPrivate>()) {
        if (!TraitItr->second.get<trait::UseAfterLoop>())
          DITrait.unset<trait::LastPrivate, trait::SecondToLastPrivate,
                        trait::DynamicPrivate>();
      } else if (TraitItr->second.get<trait::Reduction>()) {
        auto &T = *TraitItr->second.get<trait::Reduction>();
        DITrait.set<trait::Reduction>(new trait::DIReduction(T->second));
      } else {
        updateAntiFlowDep<trait::Anti>(TraitItr, DITrait);
        updateAntiFlowDep<trait::Flow>(TraitItr, DITrait);
        updateOutputDep(TraitItr, DITrait);
        if (!TraitItr->second.get<trait::Flow>())
          DITrait.unset<trait::Flow>();
        if (!TraitItr->second.get<trait::Anti>())
          DITrait.unset<trait::Anti>();
        if (!TraitItr->second.get<trait::Output>())
          DITrait.unset<trait::Output>();
      }
      if (!TraitItr->second.get<trait::UseAfterLoop>())
        DITrait.unset<trait::UseAfterLoop>();
      LLVM_DEBUG(dbgs() << "[ANALYSIS READER]: set traits to ";
                 DITrait.print(dbgs()); dbgs() << "\n");
    }
  }
  return true;
}
