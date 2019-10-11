//===--- AnalysisServer.h ------ Analysis Server ----------------*- C++ -*-===//
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
// This file implements base representation of analysis server and a server pass
// which could be used to send response.
//
// There are following steps to create and run server.
// (1) Inherit AnalysisServer class and override virtual methods,
// (2) On client:
//     (a) create socket (createAnalysisSocketImmutableStorage()),
//     (b) create server pass inherited from AnalysisServer to run server,
//     (c) wait notification from server (createAnalysisWaitServerPass()),
//     (d) after notification server is initialized and client may change
//         original module,
//     (e) use socket (getAnalysis<AnalsysisSocketImmutableWrapper>) to access
//         results of analysis in server (Socket->getAnalysis<...>(...)).
//         It is possible to use provider on server at this moment:
//           - use Socket->getAnalysis<...>(...) to get necessary passes to
//             initialize provider,
//           - initialize provider (in general way),
//           - use Socket->getAnalysis<Provider>(...) to access provider.
//     (f) close connection (createAnalysisCloseConnectionPass()).
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_SERVER_H
#define TSAR_ANALYSIS_SERVER_H

#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/AnalysisWrapperPass.h"
#include "tsar/Analysis/Passes.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/cell.h>
#include <bcl/utility.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <type_traits>

namespace llvm {
/// Initialize a wrapper pass to access mapping from a client module to
/// a server module.
void initializeAnalysisClientServerMatcherWrapperPass(PassRegistry &Registry);

/// Create a wrapper pass to access mapping from a client module to
/// a server module.
ImmutablePass * createAnalysisClientServerMatcherWrapper(
  ValueToValueMapTy &OriginalToClone);

/// Wrapper pass to access mapping from a client module to a server module.
using AnalysisClientServerMatcherWrapper =
  AnalysisWrapperPass<ValueToValueMapTy>;

/// Abstract class which implement analysis server base.
///
/// Note, that server analyzes a copy of the original module.
/// This pass run analysis server and prepares to clone module
/// (prepareToClone()), then this pass performs server initialization
/// (initializeServer()), then server passes are invoked (addServerPasses()).
/// After initialization the server notifies client (AnalysisNotifyClientPass),
/// so client should wait for this notification (AnalysisWaitServerPass) before
/// further analysis of the original module.
class AnalysisServer : public ModulePass, private bcl::Uncopyable {
public:
  explicit AnalysisServer(char &ID) : ModulePass(ID) {}

  /// Run server.
  bool runOnModule(Module &M) override {
    auto &Socket = getAnalysis<AnalysisSocketImmutableWrapper>().get();
    bcl::IntrusiveConnection::connect(&Socket, tsar::AnalysisSocket::Delimiter,
      [this, &M](bcl::IntrusiveConnection C) {
        ValueToValueMapTy CloneMap;
        prepareToClone(M, CloneMap);
        auto CloneM = CloneModule(M, CloneMap);
        legacy::PassManager PM;
        PM.add(createAnalysisConnectionImmutableWrapper(C));
        PM.add(createAnalysisClientServerMatcherWrapper(CloneMap));
        initializeServer(M, *CloneM, CloneMap, PM);
        PM.add(createAnalysisNotifyClientPass());
        addServerPasses(*CloneM, PM);
        PM.run(*CloneM);
      });
    return false;
  }

  /// Override this function if additional passes required to initialize server
  /// (see initializeServer()).
  void getAnalysisUsage(AnalysisUsage& AU) const override {
    AU.addRequired<AnalysisSocketImmutableWrapper>();
    AU.setPreservesAll();
  }

  /// Prepare to clone a specified module.
  ///
  /// For example, manual mapping of metadata could be inserted to
  /// `ClientToServer` map (by default global metadata variables and some of
  /// local variables are not cloned). This leads to implicit references to
  /// the original module. For example, traverse of MetadataAsValue for the
  /// mentioned variables visits DbgInfo intrinsics in both modules (clone and
  /// origin).
  virtual void prepareToClone(Module &ClientM,
                              ValueToValueMapTy &ClientToServer) = 0;

  /// Initialize server.
  ///
  /// The server processes a copy `ServerM` of original module `ClientM`.
  /// Correspondence between values of these modules is established in
  /// `ClientToServer` map. If some initialization passes must be run on server,
  /// add these passes to the server pass manager `PM`.
  virtual void initializeServer(Module &ClientM, Module &ServerM,
    ValueToValueMapTy &ClientToServer, legacy::PassManager &PM) = 0;

  /// Add passes to execute on server and to process requests from client.
  ///
  /// Note, AnalysisResponsePass<...> template could be used to process
  /// requests in simple cases.
  virtual void addServerPasses(Module &ServerM, legacy::PassManager &PM) = 0;
};

/// This pass waits for requests from client and send responses from server.
///
/// This pass should be run on server (use AnalysisServer::addServerPasses).
/// List of types ResponseT... specifies passes which may be interested for
/// a client.
template<class... ResponseT>
class AnalysisRespnosePass : public ModulePass, private bcl::Uncopyable {
  struct AddRequiredFunctor {
    AddRequiredFunctor(llvm::AnalysisUsage &AU) : mAU(AU) {}
    template<class AnalysisType> void operator()() {
      mAU.addRequired<typename std::remove_pointer<AnalysisType>::type>();
    }
  private:
    llvm::AnalysisUsage &mAU;
  };
public:
  static char ID;

  AnalysisRespnosePass() : ModulePass(ID) {}

  /// Wait for responses in infinite loop.
  bool runOnModule(Module &M) {
    auto &C = getAnalysis<AnalysisConnectionImmutableWrapper>();
    auto &OriginalToClone =
      getAnalysis<AnalysisClientServerMatcherWrapper>().get();
    while (C->answer([this, &OriginalToClone](
        std::string &Request) -> std::string {
      auto SpacePos = Request.find(tsar::AnalysisSocket::Space);
      StringRef RawID(Request), RawF;
      if (SpacePos != std::string::npos) {
        RawF = RawID.substr(SpacePos + 1);
        RawID = RawID.substr(0, SpacePos);
      }
      std::uintptr_t IntToPtr;
      llvm::to_integer(RawID, IntToPtr, 10);
      auto ID = reinterpret_cast<llvm::AnalysisID>(IntToPtr);
      // Use implementation of getAnalysisID() from llvm/PassAnalysisSupport.h.
      // Pass::getAnalysisID() is a template, however be do not know type of
      // required pass. So, copy body of getAnalysisID() without type cast.
      assert(getResolver() &&
             "Pass has not been inserted into a PassManager object!");
      Pass *ResultPass = nullptr;
      if (RawF.empty()) {
        ResultPass = getResolver()->findImplPass(ID);
      } else {
        llvm::to_integer(RawF, IntToPtr, 10);
        auto *F = reinterpret_cast<llvm::Function *>(IntToPtr);
        assert(F && "Function must be specified!");
        auto &CloneF = OriginalToClone[F];
        if (!CloneF)
          return "";
        ResultPass = getResolver()->findImplPass(this, ID,
                                                 *cast<llvm::Function>(CloneF));
      }
      assert(ResultPass && "getAnalysis*() called on an analysis that was not "
             "'required' by pass!");
      void *P = ResultPass->getAdjustedAnalysisPointer(ID);
      std::string PassPtr;
      llvm::raw_string_ostream OS(PassPtr);
      OS << reinterpret_cast<uintptr_t>(P);
      return OS.str();
    }));
    return false;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AnalysisConnectionImmutableWrapper>();
    AU.addRequired<AnalysisClientServerMatcherWrapper>();
    AddRequiredFunctor AddRequired(AU);
    bcl::TypeList<ResponseT...>::for_each_type(AddRequired);
    AU.setPreservesAll();
  }
};
}
#endif//TSAR_ANALYSIS_SERVER_H
