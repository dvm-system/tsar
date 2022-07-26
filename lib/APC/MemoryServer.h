#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Memory/TraitFilter.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Verifier.h>

#ifndef MEMORY_SERVER_H
#define MEMORY_SERVER_H

namespace llvm {
void initializeDVMHMemoryServerPass(PassRegistry &);
void initializeDVMHMemoryServerResponsePass(PassRegistry &);
void initializeInitializeProviderPassPass(PassRegistry &);

/// This provides access to function-level analysis results on server.
using DVMHMemoryServerProvider =
    FunctionPassAAProvider<DelinearizationPass, DominatorTreeWrapperPass,
                           EstimateMemoryPass, DIEstimateMemoryPass,
                           DIDependencyAnalysisPass>;

/// List of responses available from server (client may request corresponding
/// analysis, in case of provider all analysis related to a provider may
/// be requested separately).
using DVMHMemoryServerResponse = AnalysisResponsePass<
    GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
    GlobalDefinedMemoryWrapper, GlobalLiveMemoryWrapper, GlobalsAccessWrapper,
    ClonedDIMemoryMatcherWrapper, DVMHMemoryServerProvider,
    DIArrayAccessWrapper>;

class InitializeProviderPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  InitializeProviderPass() : ModulePass(ID) {
    initializeInitializeProviderPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    DVMHMemoryServerProvider::initialize<GlobalOptionsImmutableWrapper>(
        [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
          Wrapper.setOptions(&GO);
        });
    auto &DIMEnv = getAnalysis<DIMemoryEnvironmentWrapper>().get();
    DVMHMemoryServerProvider::initialize<DIMemoryEnvironmentWrapper>(
        [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) {
          Wrapper.set(DIMEnv);
        });
    auto &DIMTraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
    DVMHMemoryServerProvider::initialize<DIMemoryTraitPoolWrapper>(
        [&DIMTraitPool](DIMemoryTraitPoolWrapper &Wrapper) {
          Wrapper.set(DIMTraitPool);
        });
    auto &GlobalsAA = getAnalysis<GlobalsAAWrapperPass>().getResult();
    DVMHMemoryServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
        [&GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
          Wrapper.set(GlobalsAA);
        });
    auto &GlobalDefUse = getAnalysis<GlobalDefinedMemoryWrapper>().get();
    DVMHMemoryServerProvider::initialize<GlobalDefinedMemoryWrapper>(
      [&GlobalDefUse](GlobalDefinedMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalDefUse);
      });
    auto &GlobalLiveMemory = getAnalysis<GlobalLiveMemoryWrapper>().get();
    DVMHMemoryServerProvider::initialize<GlobalLiveMemoryWrapper>(
      [&GlobalLiveMemory](GlobalLiveMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalLiveMemory);
      });
    if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
      DVMHMemoryServerProvider::initialize<GlobalsAccessWrapper>(
          [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<GlobalOptionsImmutableWrapper>();
    AU.addRequired<GlobalsAAWrapperPass>();
    AU.addRequired<DIMemoryEnvironmentWrapper>();
    AU.addRequired<DIMemoryTraitPoolWrapper>();
    AU.addRequired<GlobalsAccessWrapper>();
    AU.addRequired<GlobalDefinedMemoryWrapper>();
    AU.addRequired<GlobalLiveMemoryWrapper>();
    AU.setPreservesAll();
  }
};

class DVMHMemoryServer final : public AnalysisServer {
public:
  static char ID;
  DVMHMemoryServer() : AnalysisServer(ID) {
    initializeDVMHMemoryServerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AnalysisServer::getAnalysisUsage(AU);
    tsar::ClientToServerMemory::getAnalysisUsage(AU);
    AU.addRequired<GlobalOptionsImmutableWrapper>();
  }

  void prepareToClone(llvm::Module &ClientM,
                      ValueToValueMapTy &ClientToServer) override {
    tsar::ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
  }

  void initializeServer(llvm::Module &CM, llvm::Module &SM,
       ValueToValueMapTy &CToS, legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
    PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
    PM.add(createGlobalDefinedMemoryStorage());
    PM.add(createGlobalLiveMemoryStorage());
    PM.add(createDIMemoryTraitPoolStorage());
    PM.add(createDIArrayAccessStorage());
    tsar::ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
  }

  void addServerPasses(llvm::Module &M, legacy::PassManager &PM) override {
    using namespace tsar;
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    addImmutableAliasAnalysis(PM);
    addBeforeTfmAnalysis(PM);
    addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
    PM.add(createDIArrayAccessCollector());
    PM.add(createAnalysisNotifyClientPass());
    PM.add(new InitializeProviderPass);
    PM.add(new DVMHMemoryServerResponse);
    addAfterFunctionInlineAnalysis(
        GO, M.getDataLayout(),
        [](auto &T) {
          unmarkIf<trait::Lock, trait::NoPromotedScalar>(T);
          unmark<trait::NoPromotedScalar>(T);
        },
        PM);
    addAfterLoopRotateAnalysis(PM);
    PM.add(createAnalysisNotifyClientPass());
    PM.add(createVerifierPass());
    PM.add(new InitializeProviderPass);
    PM.add(new DVMHMemoryServerResponse);
  }

  void prepareToClose(legacy::PassManager &PM) override {
    tsar::ClientToServerMemory::prepareToClose(PM);
  }
};

}
#endif//MEMORY_SERVER_H
