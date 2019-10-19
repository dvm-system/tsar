//===--- AnalysisSocket.cpp ---- Analysis Socket ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file implements a socket which allows to establish connection to
// analysis server and to obtain analysis results and to perform synchronization
// between a client and a server.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Passes.h"

using namespace llvm;
using namespace tsar;

namespace {
class AnalysisSocketImmutableStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;

  AnalysisSocketImmutableStorage() : ImmutablePass(ID) {
    initializeAnalysisSocketImmutableStoragePass(
      *PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<AnalysisSocketImmutableWrapper>().set(mSocket);
  }

  void getAnalysisUsage(AnalysisUsage& AU) const override {
    AU.addRequired<AnalysisSocketImmutableWrapper>();
  }

private:
  AnalysisSocket mSocket;
};

class AnalysisNotifyClientPass :
  public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  AnalysisNotifyClientPass() : ModulePass(ID) {
    initializeAnalysisNotifyClientPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    auto &Connection = getAnalysis<AnalysisConnectionImmutableWrapper>();
    bool IsNotified = false;
    // Ignore all requests except 'wait'.
    while (!IsNotified &&
           Connection->answer(
               [&IsNotified](const std::string &Request) -> std::string {
                 if (Request == AnalysisSocket::Wait) {
                   AnalysisResponse Response;
                   Response[AnalysisResponse::Analysis].push_back(
                       DenseMapInfo<void *>::getTombstoneKey());
                   IsNotified = true;
                   return json::Parser<AnalysisResponse>::unparseAsObject(
                       Response);
                 }
                 return "";
               }))
      ;
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AnalysisConnectionImmutableWrapper>();
    AU.setPreservesAll();
  }
};

class AnalysisWaitServerPass :
  public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  AnalysisWaitServerPass() : ModulePass(ID) {
    initializeAnalysisWaitServerPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module& M) override {
    auto &Socket = getAnalysis<AnalysisSocketImmutableWrapper>();
    Socket->wait();
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AnalysisSocketImmutableWrapper>();
    AU.setPreservesAll();
  }
};

class AnalysisCloseConnectionPass :
  public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  AnalysisCloseConnectionPass() : ModulePass(ID) {
    initializeAnalysisCloseConnectionPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module& M) override {
    auto &Socket = getAnalysis<AnalysisSocketImmutableWrapper>();
    Socket->close();
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AnalysisSocketImmutableWrapper>();
    AU.setPreservesAll();
  }
};
}

char AnalysisSocketImmutableStorage::ID = 0;
INITIALIZE_PASS_BEGIN(AnalysisSocketImmutableStorage, "analysis-socket-is",
  "Analysis Thread (Socket Immutable Storage)", true, true)
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
INITIALIZE_PASS_END(AnalysisSocketImmutableStorage, "analysis-socket-is",
  "Analysis Thread (Socket Immutable Storage)", true, true)

template<> char AnalysisSocketImmutableWrapper::ID = 0;
INITIALIZE_PASS(AnalysisSocketImmutableWrapper, "analysis-socket-iw",
  "Analysis Thread (Socket Immutable Wrapper)", true, true)

template<> char AnalysisConnectionImmutableWrapper::ID = 0;
INITIALIZE_PASS(AnalysisConnectionImmutableWrapper, "analysis-connection-iw",
  "Analysis Thread (Connection Immutable Wrapper)", true, true)

char AnalysisNotifyClientPass::ID = 0;
INITIALIZE_PASS_BEGIN(AnalysisNotifyClientPass, "analysis-notify",
  "Analysis Thread (Notification)", true, true)
INITIALIZE_PASS_DEPENDENCY(AnalysisConnectionImmutableWrapper)
INITIALIZE_PASS_END(AnalysisNotifyClientPass, "analysis-notify",
  "Analysis Thread (Notification)", true, true)

char AnalysisWaitServerPass::ID = 0;
INITIALIZE_PASS_BEGIN(AnalysisWaitServerPass, "analysis-wait",
  "Analysis Thread (Wait)", true, true)
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
INITIALIZE_PASS_END(AnalysisWaitServerPass, "analysis-wait",
  "Analysis Thread (Wait)", true, true)

char AnalysisCloseConnectionPass::ID = 0;
INITIALIZE_PASS_BEGIN(AnalysisCloseConnectionPass, "analysis-socket-close",
  "Analysis Thread (Close)", true, true)
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
INITIALIZE_PASS_END(AnalysisCloseConnectionPass, "analysis-socket-close",
  "Analysis Thread (Close)", true, true)

ImmutablePass * llvm::createAnalysisSocketImmutableStorage() {
  return new AnalysisSocketImmutableStorage;
}

ImmutablePass * llvm::createAnalysisConnectionImmutableWrapper(
    bcl::IntrusiveConnection &C) {
  // AnalysisWrapperPass template does not call initialization function in
  // constructor, so invoke it here manually.
  initializeAnalysisConnectionImmutableWrapperPass(
    *PassRegistry::getPassRegistry());
  auto P = new AnalysisConnectionImmutableWrapper;
  P->set(C);
  return P;
}

ModulePass * llvm::createAnalysisNotifyClientPass() {
  return new AnalysisNotifyClientPass;
}

ModulePass * llvm::createAnalysisWaitServerPass() {
  return new AnalysisWaitServerPass;
}

ModulePass * llvm::createAnalysisCloseConnectionPass() {
  return new AnalysisCloseConnectionPass;
}
