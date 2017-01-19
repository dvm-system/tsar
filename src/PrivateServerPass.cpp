//===- PrivateServerPass.cpp -- Test Result Printer -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass to interact with client software and to provide
//
//===----------------------------------------------------------------------===//

#include <llvm/ADT/StringSwitch.h>
#include <llvm/Analysis/Passes.h>
#if (LLVM_VERSION_MAJOR > 2 && LLVM_VERSION_MINOR > 7)
# include <llvm/Analysis/BasicAliasAnalysis.h>
#endif
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>
#include <cell.h>
#include <IntrusiveConnection.h>
#include <Json.h>
#include <RedirectIO.h>
#include "DFRegionInfo.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "Messages.h"
#include "tsar_pass_provider.h"
#include "tsar_private.h"
#include "PrivateServerPass.h"
#include "tsar_trait.h"
#include "tsar_transformation.h"

using namespace llvm;
using namespace tsar;
using ::llvm::Module;

#undef DEBUG_TYPE
#define DEBUG_TYPE "server-private"

namespace tsar {
namespace msg {
namespace detail {
struct Statistic {
  MSG_FIELD_TYPE(Files, std::map<BCL_JOIN(std::string, unsigned)>)
  MSG_FIELD_TYPE(Functions, unsigned)
  MSG_FIELD_TYPE(Loops, std::map<BCL_JOIN(Analysis, unsigned)>)
  MSG_FIELD_TYPE(Variables, std::map<BCL_JOIN(Analysis, unsigned)>)
  MSG_FIELD_TYPE(Traits,
    bcl::StaticTraitMap<BCL_JOIN(unsigned, DependencyDescriptor)>)
  typedef bcl::StaticMap<Functions, Variables, Files, Loops, Traits> Base;
};
}

/// \brief This message provides statistic of program analysis results.
///
/// This contains number of analyzed files, functions, loops and variables and
/// number of explored traits, such as induction, reduction, data dependencies,
/// etc.
class Statistic :
  public json::Object, public msg::detail::Statistic::Base {
  struct InitTraitsFunctor {
    template<class Trait> inline void operator()(unsigned &C) { C = 0; }
  };
public:
  MSG_NAME(Statistic)
  MSG_FIELD(Statistic, Files)
  MSG_FIELD(Statistic, Functions)
  MSG_FIELD(Statistic, Loops)
  MSG_FIELD(Statistic, Variables)
  MSG_FIELD(Statistic, Traits)

  Statistic() : json::Object(name()), msg::detail::Statistic::Base(0) {
    (*this)[Statistic::Traits].for_each(InitTraitsFunctor());
  }

  Statistic(const Statistic &) = default;
  Statistic & operator=(const Statistic &) = default;
  Statistic(Statistic &&) = default;
  Statistic & operator=(Statistic &&) = default;
};
}
}

namespace json {
template<> struct Traits<tsar::msg::Statistic> :
  public json::Traits<tsar::msg::detail::Statistic::Base> {};
}

#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
namespace {
class ServerPrivateProvider : public FunctionPassProvider<
  PrivateRecognitionPass, TransformationEnginePass,
  LoopMatcherPass, DFRegionInfoPass> {
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    auto P = createBasicAliasAnalysisPass();
    AU.addRequiredID(P->getPassID());
    delete P;
    FunctionPassProvider::getAnalysisUsage(AU);
  }
};
}
#else
typedef FunctionPassProvider<
  BasicAAWrapperPass,
  PrivateRecognitionPass,
  TransformationEnginePass,
  LoopMatcherPass,
  DFRegionInfoPass> ServerPrivateProvider;
#endif

INITIALIZE_PROVIDER_BEGIN(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PROVIDER_END(ServerPrivateProvider, "server-private-provider",
  "Server Private Provider")

char PrivateServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(PrivateServerPass, "server-private",
  "Server Private Pass", true, true)
INITIALIZE_PASS_DEPENDENCY(ServerPrivateProvider)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(PrivateServerPass, "test-printer",
  "Server Private Pass", true, true)

namespace {
/// Increments count of analyzed traits in a specified map TM.
template<class TraitMap>
void incrementTraitCount(Function &F, ServerPrivateProvider &P, TraitMap &TM) {
  auto &LMP = P.get<LoopMatcherPass>();
  auto &PI = P.get<PrivateRecognitionPass>().getPrivateInfo();
  auto &RI = P.get<DFRegionInfoPass>().getRegionInfo();
  for (auto &Match : LMP.getMatcher()) {
    auto N = RI.getRegionFor(Match.get<IR>());
    auto DSItr = PI.find(N);
    assert(DSItr != PI.end() && DSItr->get<DependencySet>() &&
      "Privatiability information must be specified!");
    for (auto &TS : *DSItr->get<DependencySet>())
      TS.for_each(
        bcl::TraitMapConstructor<
        LocationTraitSet, TraitMap, bcl::CountInserter>(TS, TM));
  }
}
}

bool PrivateServerPass::runOnModule(llvm::Module &M) {
  if (!mConnection) {
    errs() << "error: intrusive connection is not specified for the module "
      << M.getName() << "\n";
    return false;
  }
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return false;
  }
  ServerPrivateProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
      TEP.setContext(M, TfmCtx);
  });
  mConnection->answer(
      [this, &M, &TfmCtx](const std::string &Request) -> std::string {
    msg::Diagnostic Diag(msg::Status::Error);
    if (mStdErr->isDiff()) {
      Diag[msg::Diagnostic::Terminal] += mStdErr->diff();
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    msg::Statistic Stat;
    json::Parser<> P(Request);
    if (!P.parse(Stat)) {
      Diag.insert(msg::Diagnostic::Error, P.errors());
      return json::Parser<msg::Diagnostic>::unparseAsObject(Diag);
    }
    auto &Rewriter = TfmCtx->getRewriter();
    for (auto FI = Rewriter.getSourceMgr().fileinfo_begin(),
         EI = Rewriter.getSourceMgr().fileinfo_end(); FI != EI; ++FI) {
      auto Ext = sys::path::extension(FI->first->getName());
      std::string Kind;
      if (std::find(
            msg::HeaderFile::extensions().begin(),
            msg::HeaderFile::extensions().end(), Ext) !=
          msg::HeaderFile::extensions().end())
        Kind = msg::HeaderFile::name();
      else if (std::find(
            msg::SourceFile::extensions().begin(),
            msg::SourceFile::extensions().end(), Ext) !=
          msg::SourceFile::extensions().end())
        Kind = msg::SourceFile::name();
      else
        Kind = msg::OtherFile::name();
      auto I = Stat[msg::Statistic::Files].insert(std::make_pair(Kind, 1));
      if (!I.second)
        ++I.first->second;
    }
    auto &MMP = getAnalysis<MemoryMatcherPass>();
    Stat[msg::Statistic::Variables].insert(
      std::make_pair(msg::Analysis::Yes, MMP.getMatcher().size()));
    Stat[msg::Statistic::Variables].insert(
      std::make_pair(msg::Analysis::No, MMP.getUnmatchedAST().size()));
    std::pair<unsigned, unsigned> Loops(0, 0);
    for (Function &F : M) {
      if (F.empty())
        continue;
      ++Stat[msg::Statistic::Functions];
      auto &Provider = getAnalysis<ServerPrivateProvider>(F);
      auto &LMP = Provider.get<LoopMatcherPass>();
      Loops.first += LMP.getMatcher().size();
      Loops.second += LMP.getUnmatchedAST().size();
      incrementTraitCount(F, Provider, Stat[msg::Statistic::Traits]);
    }
    Stat[msg::Statistic::Loops].insert(
      std::make_pair(msg::Analysis::Yes, Loops.first));
    Stat[msg::Statistic::Loops].insert(
      std::make_pair(msg::Analysis::No, Loops.second));
    return json::Parser<msg::Statistic>::unparseAsObject(Stat);
  });
  return false;
}

void PrivateServerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ServerPrivateProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherPass>();
  AU.setPreservesAll();
}

ModulePass * llvm::createPrivateServerPass(
    bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr) {
  return new PrivateServerPass(IC, StdErr);
}
