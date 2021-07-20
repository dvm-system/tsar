//=== DIMemoryTraitServer.cpp - Metadata-level Analysis Server --*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file defines general analysis server o obtain memory traits for program
// regions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/DIMemoryTrait.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Memory/TraitFilter.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include <llvm/InitializePasses.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;

namespace llvm {
static void initializeDIMemoryAnalysisServerProviderPassPass(PassRegistry &);
static void initializeDIMemoryAnalysisServerResponsePass(PassRegistry &);
}

namespace {
/// This provides access to function-level analysis results on server.
using DIMemoryAnalysisServerProvider =
FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;

/// List of responses available from server (client may request corresponding
/// analysis, in case of provider all analysis related to a provider may
/// be requested separately).
using DIMemoryAnalysisServerResponse = AnalysisResponsePass<
  GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
  GlobalDefinedMemoryWrapper, GlobalLiveMemoryWrapper,
  ClonedDIMemoryMatcherWrapper, DIMemoryAnalysisServerProvider,
  DIArrayAccessWrapper>;

class DIMemoryAnalysisServerProviderPass : public ModulePass,
                                           private bcl::Uncopyable {
public:
  static char ID;

  DIMemoryAnalysisServerProviderPass() : ModulePass(ID) {
    initializeDIMemoryAnalysisServerProviderPassPass(
      *PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    DIMemoryAnalysisServerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GO);
      });
    auto &DIMEnv = getAnalysis<DIMemoryEnvironmentWrapper>().get();
    DIMemoryAnalysisServerProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(DIMEnv);
      });
    auto &DIMTraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
    DIMemoryAnalysisServerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [&DIMTraitPool](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(DIMTraitPool);
      });
    auto &GlobalsAA = getAnalysis<GlobalsAAWrapperPass>().getResult();
    DIMemoryAnalysisServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [&GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAA);
      });
    auto &GlobalDefUse = getAnalysis<GlobalDefinedMemoryWrapper>().get();
    DIMemoryAnalysisServerProvider::initialize<GlobalDefinedMemoryWrapper>(
      [&GlobalDefUse](GlobalDefinedMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalDefUse);
      });
    auto &GlobalLiveMemory = getAnalysis<GlobalLiveMemoryWrapper>().get();
    DIMemoryAnalysisServerProvider::initialize<GlobalLiveMemoryWrapper>(
      [&GlobalLiveMemory](GlobalLiveMemoryWrapper &Wrapper) {
        Wrapper.set(GlobalLiveMemory);
      });
    if (auto &GAP = getAnalysis<GlobalsAccessWrapper>())
      DIMemoryAnalysisServerProvider::initialize<GlobalsAccessWrapper>(
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

/// This analysis server performs transformation-based analysis which is
/// necessary to answer user requests.
class DIMemoryAnalysisServer final : public AnalysisServer {
public:
  static char ID;
  DIMemoryAnalysisServer() : AnalysisServer(ID) {
    initializeDIMemoryAnalysisServerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AnalysisServer::getAnalysisUsage(AU);
    tsar::ClientToServerMemory::getAnalysisUsage(AU);
    AU.addRequired<GlobalOptionsImmutableWrapper>();
  }

  void prepareToClone(Module &ClientM,
    ValueToValueMapTy &ClientToServer) override {
    ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
  }

  void initializeServer(Module &CM, Module &SM, ValueToValueMapTy &CToS,
    legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
    PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
    PM.add(createGlobalDefinedMemoryStorage());
    PM.add(createGlobalLiveMemoryStorage());
    PM.add(createDIMemoryTraitPoolStorage());
    PM.add(createDIArrayAccessStorage());
    ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
  }

  void addServerPasses(Module &M, legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    addImmutableAliasAnalysis(PM);
    addBeforeTfmAnalysis(PM);
    addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
    PM.add(createDIArrayAccessCollector());
    addAfterFunctionInlineAnalysis(
        GO, M.getDataLayout(),
        [](auto &T) {
          unmarkIf<trait::Lock, trait::NoPromotedScalar>(T);
          unmark<trait::NoPromotedScalar>(T);
        },
        PM);
    addAfterLoopRotateAnalysis(PM);
    // Notify client that analysis is performed. Analysis changes metadata-level
    // alias tree and invokes corresponding handles to update client to server
    // mapping. So, metadata-level memory mapping is a shared resource and
    // synchronization is necessary.
    PM.add(createAnalysisNotifyClientPass());
    PM.add(createVerifierPass());
    PM.add(new DIMemoryAnalysisServerProviderPass);
    PM.add(new DIMemoryAnalysisServerResponse);
  }

  void prepareToClose(legacy::PassManager &PM) override {
    ClientToServerMemory::prepareToClose(PM);
  }
};
}

ModulePass *llvm::createDIMemoryAnalysisServer() {
  return new DIMemoryAnalysisServer;
}

INITIALIZE_PROVIDER(DIMemoryAnalysisServerProvider, "di-memory-server-provider",
  "Metadata-Level Memory Server (Provider)")

  char DIMemoryAnalysisServer::ID = 0;
INITIALIZE_PASS_BEGIN(DIMemoryAnalysisServer, "di-memory-server",
  "Metadata-Level Memory Server", false, false)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryAnalysisServerProvider)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryAnalysisServerResponse)
  INITIALIZE_PASS_END(DIMemoryAnalysisServer, "di-memory-server",
    "Metadata-Level Memory Server", false, false)

  template <> char DIMemoryAnalysisServerResponse::ID = 0;
INITIALIZE_PASS_BEGIN(DIMemoryAnalysisServerResponse,
  "di-memory-server-response",
  "Metadata-Level Memory Server (Response)", true, false)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryAnalysisServerProvider)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
  INITIALIZE_PASS_END(DIMemoryAnalysisServerResponse,
    "di-memory-server-response",
    "Metadata-Level Memory Server (Response)", true, false)

  char DIMemoryAnalysisServerProviderPass::ID = 0;
INITIALIZE_PASS_BEGIN(DIMemoryAnalysisServerProviderPass,
  "di-memory-server-provider-init",
  "Metadata-Level Memory Server (Provider, Initialize)", true, true)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
  INITIALIZE_PASS_END(DIMemoryAnalysisServerProviderPass,
    "di-memory-server-provider-init",
    "Metadata-Level Memory Server (Provider, Initialize)", true, true)
