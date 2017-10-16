//===--- tsar_private.h - Private Variable Analyzer -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines passes to determine locations which can be privatized.
// We use data-flow framework to implement this kind of analysis. This file
// contains elements which is necessary to determine this framework.
// The following articles can be helpful to understand it:
//  * "Automatic Array Privatization" Peng Tu and David Padua
//  * "Array Privatization for Parallel Execution of Loops" Zhiyuan Li.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRIVATE_H
#define TSAR_PRIVATE_H

#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_df_graph.h"
#include "tsar_df_location.h"
#include "DefinedMemory.h"
#include "LiveMemory.h"
#include "tsar_pass.h"
#include "tsar_trait.h"
#include "tsar_utility.h"

namespace tsar {
class DefUseSet;
class DFLoop;

/// Information about privatizability of locations for an analyzed region.
typedef llvm::DenseMap<DFNode *, std::unique_ptr<DependencySet>,
  llvm::DenseMapInfo<DFNode *>,
  TaggedDenseMapPair<
    bcl::tagged<DFNode *, DFNode>,
    bcl::tagged<std::unique_ptr<DependencySet>, DependencySet>>> PrivateInfo;
}

namespace llvm {
class AliasSetTracker;
class Loop;

/// This pass determines locations which can be privatized.
class PrivateRecognitionPass :
    public FunctionPass, private bcl::Uncopyable {
  /// Map from base location to traits.
  typedef DenseMap<const MemoryLocation *, unsigned long long> TraitMap;
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  PrivateRecognitionPass() : FunctionPass(ID) {
    initializePrivateRecognitionPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about privatizability of locations for an analyzed
  /// region.
  tsar::PrivateInfo & getPrivateInfo() noexcept {return mPrivates;}

  /// Returns information about privatizability of locations for an analyzed
  /// region.
  const tsar::PrivateInfo & getPrivateInfo() const noexcept {return mPrivates;}

  /// Recognizes private (last private) variables for loops
  /// in the specified function.
  /// \pre A control-flow graph of the specified function must not contain
  /// unreachable nodes.
  bool runOnFunction(Function &F) override;

  /// Releases allocated memory.
  void releaseMemory() override {
    mPrivates.clear();
    mDefInfo = nullptr;
    mLiveInfo = nullptr;
  }

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Prints out the internal state of the pass. This also used to produce
  /// analysis correctness tests.
  void print(raw_ostream &OS, const Module *M) const override;

private:
  /// \brief Implements recognition of privatizable locations.
  ///
  /// Privatizability analysis is performed in two steps. Firstly,
  /// body of each natural loop is analyzed. Secondly, when live locations
  /// for each basic block are discovered, results of loop body analysis must be
  /// finalized. The result of this analysis should be complemented to separate
  /// private from last private locations. The case where location access
  /// is performed by pointer is also considered. Shared locations also
  /// analyzed.
  /// \param [in, out] R Region in a data-flow graph, it can not be null.
  /// \pre Results of live memory analysis and reach definition analysis
  /// must be available from mLiveInfo and mDefInfo.
  void resolveCandidats(tsar::DFRegion *R);

  /// Evaluates explicitly accessed variables in a loop.
  void resolveAccesses(const tsar::DFNode *LatchNode,
    const tsar::DFNode *ExitNode, const tsar::DefUseSet &DefUse,
    const tsar::LiveSet &LS, TraitMap &LocBases, tsar::DependencySet &DS);

  /// Evaluates cases when location access is performed by pointer in a loop.
  void resolvePointers(const tsar::DefUseSet &DefUse, TraitMap &LocBases,
    tsar::DependencySet &DS);

  /// Store results for subsequent passes.
  ///
  /// \attention This method can update LocBases, so use with caution
  /// methods which read LocBases before this method.
  void storeResults(TraitMap &LocBases, tsar::DependencySet &DS);

  /// \brief Recognizes addresses of locations which is evaluated in a loop a
  /// for which need to pay attention during loop transformation.
  ///
  /// In the following example the variable X can be privatized, but address
  /// of the original variable X should be available after transformation.
  /// \code
  /// int X;
  /// for (...)
  ///   ... = &X;
  /// ..X = ...;
  /// \endcode
  void resolveAddresses(tsar::DFLoop *L, const tsar::DefUseSet &DefUse,
    TraitMap &LocBases, tsar::DependencySet &DS);

private:
  tsar::PrivateInfo mPrivates;
  AliasSetTracker *mAliasTracker = nullptr;
  const tsar::DefinedMemoryInfo *mDefInfo = nullptr;
  const tsar::LiveMemoryInfo *mLiveInfo = nullptr;
};
}
#endif//TSAR_PRIVATE_H
