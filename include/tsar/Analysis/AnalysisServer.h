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
// (1) Inherit AnalysisServer class and override virtual methods. The last pass
//     executed on the server notifies the client that connection can be closed.
//     So, before this pass all shared data should be freed, for example all
//     handles should be destroyed to avoid undefined behavior.
// (2) On client:
//     (a) create socket (createAnalysisSocketImmutableStorage()),
//     (b) create server pass inherited from AnalysisServer to run server,
//     (c) wait notification from server (createAnalysisWaitServerPass()),
//     (d) after notification server is initialized and client may change
//         original module,
//     (e) use socket (getAnalysis<AnalsysisSocketImmutableWrapper>) to access
//         results of analysis in server (Socket->getAnalysis<...>(...)).
//         It is possible to use provider on server at this moment:
//           - use Socket->getAnalysis<...>() to get necessary passes to
//             initialize provider,
//           - initialize provider (in general way),
//           - use Socket->getAnalysis<...>(F) to access passes from provider.
//             This function looks up for an appropriate provider which contains
//             all required passes and returns related analysis results. It is
//             also possible to access provider explicitly
//             Socket->getAnalysis<...>(F).
//     (f) notify server that all analysis requests have been received
//         (createAnalysisReleaseServerPass()),
//     (g) close connection (createAnalysisCloseConnectionPass()). Client will
//         be blocked until server confirms that connection can be closed.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_SERVER_H
#define TSAR_ANALYSIS_SERVER_H

#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/PassProvider.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/cell.h>
#include <bcl/utility.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/STLExtras.h>
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
ImmutablePass *
createAnalysisClientServerMatcherWrapper(ValueToValueMapTy &OriginalToClone);

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
    auto &SocketInfo = getAnalysis<AnalysisSocketImmutableWrapper>().get();
    auto &Socket = SocketInfo.emplace(getPassID(), true).first->second;
    bcl::IntrusiveConnection::connect(
        &Socket, tsar::AnalysisSocket::Delimiter,
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
          prepareToClose(PM);
          PM.add(createAnalysisNotifyClientPass());
          PM.run(*CloneM);
        });
    return false;
  }

  /// Override this function if additional passes required to initialize server
  /// (see initializeServer()).
  void getAnalysisUsage(AnalysisUsage &AU) const override {
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
                                ValueToValueMapTy &ClientToServer,
                                legacy::PassManager &PM) = 0;

  /// Add passes to execute on server and to process requests from client.
  ///
  /// Note, AnalysisResponsePass<...> template could be used to process
  /// requests in simple cases.
  virtual void addServerPasses(Module &ServerM, legacy::PassManager &PM) = 0;

  /// Add passes to execute until connection is not closed, for example
  /// shared data are freed.
  virtual void prepareToClose(legacy::PassManager & PM) = 0;
};

/// This pass waits for requests from client and send responses from server.
///
/// This pass should be run on server (use AnalysisServer::addServerPasses).
/// List of types ResponseT... specifies passes which may be interested for
/// a client.
template <class... ResponseT>
class AnalysisResponsePass : public ModulePass, private bcl::Uncopyable {

  struct AddRequiredFunctor {
    AddRequiredFunctor(llvm::AnalysisUsage &AU) : mAU(AU) {}
    template <class AnalysisType> void operator()() {
      mAU.addRequired<typename std::remove_pointer<AnalysisType>::type>();
    }

  private:
    llvm::AnalysisUsage &mAU;
  };

  /// Look up for analysis with a specified ID in a list of analysis.
  ///
  /// Set `Exists` to true if analysis is found.
  struct FindAnalysis {
    template <class T> void operator()() { Exist |= (ID == &T::ID); }
    AnalysisID ID;
    bool &Exist;
  };

  /// List of analysis results (ID to Analysis).
  using AnalysisCache =
      llvm::SmallVector<std::pair<llvm::AnalysisID, void *>, 8>;

  /// Get all available analysis from a specified provider and store their
  /// into a specified cache.
  template <class ProviderT> struct GetAllFromProvider {
    template <class T> void operator()() {
      auto &P = Provider.template get<T>();
      Cache.emplace_back(&T::ID, static_cast<void *>(&P));
    }
    ProviderT &Provider;
    AnalysisCache &Cache;
  };

  /// This functor looks up for a provider which allows to access required
  /// analysis mentioned in AnalysisRequest.
  ///
  /// If provider is found results of required analysis will be stored in
  /// analysis response.
  struct FindProvider {
    /// Perform search while response is empty.
    template <class T> void operator()() {
      if (Response[tsar::AnalysisResponse::Analysis].empty())
        processAsProvider<T>(tsar::is_pass_provider<T>());
    }

    /// Process `T` as a provider.
    template <class T> void processAsProvider(std::true_type) {
      bool ExistInProvider = true;
      for (auto &ID : Request[tsar::AnalysisRequest::AnalysisIDs]) {
        bool E = false;
        tsar::pass_provider_analysis<T>::for_each_type(FindAnalysis{ID, E});
        ExistInProvider &= E;
      }
      if (ExistInProvider) {
        auto &Provider = This->getAnalysis<T>(CloneF);
        for (auto &ID : Request[tsar::AnalysisRequest::AnalysisIDs]) {
          Response[tsar::AnalysisResponse::Analysis].push_back(
              Provider.getWithID(ID));
        }
        tsar::pass_provider_analysis<T>::for_each_type(
            GetAllFromProvider<T>{Provider, Cache});
      }
    }

    /// Do not process `T` as a provider.
    template <class T> void processAsProvider(std::false_type) {}

    AnalysisResponsePass<ResponseT...> *This;
    Function &CloneF;
    tsar::AnalysisRequest &Request;
    tsar::AnalysisResponse &Response;
    AnalysisCache &Cache;
  };

public:
  static char ID;

  AnalysisResponsePass() : ModulePass(ID) {}

  /// Wait for requests in infinite loop. Stop waiting after incorrect request.
  bool runOnModule(Module &M) {
    auto &C = getAnalysis<AnalysisConnectionImmutableWrapper>();
    auto &OriginalToClone =
        getAnalysis<AnalysisClientServerMatcherWrapper>().get();
    bool WaitForRequest = true;
    llvm::Function *ActiveFunc = nullptr;
    AnalysisCache ActiveIDs;
    while (WaitForRequest && C->answer([this, &OriginalToClone, &ActiveFunc,
                                        &ActiveIDs,
                                        &WaitForRequest](std::string &Request)
                                           -> std::string {
      if (Request == tsar::AnalysisSocket::Release) {
        WaitForRequest = false;
        return { tsar::AnalysisSocket::Notify };
      }
      ::json::Parser<tsar::AnalysisRequest> Parser(Request);
      tsar::AnalysisRequest R;
      if (!Parser.parse(R)) {
        llvm_unreachable("Unknown request: listen for analysis request!");
        return { tsar::AnalysisSocket::Invalid };
      }
      tsar::AnalysisResponse Response;
      if (auto *F = R[tsar::AnalysisRequest::Function]) {
        auto &CloneF = OriginalToClone[F];
        if (!CloneF)
          return { tsar::AnalysisSocket::Data};
        // Check whether we already have required analysis.
        if (ActiveFunc == &*CloneF) {
          for (auto ID : R[tsar::AnalysisRequest::AnalysisIDs]) {
            auto Itr =
                llvm::find_if(ActiveIDs, [ID](AnalysisCache::value_type &V) {
                  return V.first == ID;
                });
            if (Itr == ActiveIDs.end()) {
              Response[tsar::AnalysisResponse::Analysis].clear();
              ActiveIDs.clear();
              break;
            }
            Response[tsar::AnalysisResponse::Analysis].push_back(Itr->second);
          }
        } else {
          ActiveFunc = cast<Function>(CloneF);
        }
        if (Response[tsar::AnalysisResponse::Analysis].empty()) {
          // If only one function-level analysis is required, then try to find
          // it in the list of available responses (ResponseT...). Otherwise,
          // try to find in the list of providers which provides
          // access to required analysis results.
          if (R[tsar::AnalysisRequest::AnalysisIDs].size() == 1) {
            auto ID = R[tsar::AnalysisRequest::AnalysisIDs].front();
            bool E = false;
            bcl::TypeList<ResponseT...>::for_each_type(FindAnalysis{ID, E});
            if (E) {
              auto ResultPass = getResolver()->findImplPass(
                  this, ID, *cast<Function>(CloneF));
              assert(std::get<Pass *>(ResultPass) && "getAnalysis*() called on "
                "an analysis that was not 'required' by pass!");
              Response[tsar::AnalysisResponse::Analysis].push_back(
                  std::get<Pass *>(ResultPass)->getAdjustedAnalysisPointer(ID));
              ActiveIDs.emplace_back(
                  ID, Response[tsar::AnalysisResponse::Analysis].back());
            }
          }
          if (Response[tsar::AnalysisResponse::Analysis].empty()) {
            FindProvider FindImpl{this, *cast<Function>(CloneF), R, Response,
                                  ActiveIDs};
            bcl::TypeList<ResponseT...>::for_each_type(FindImpl);
            if (Response[tsar::AnalysisResponse::Analysis].empty())
              return {tsar::AnalysisSocket::Data};
          }
        }
      } else {
        // Use implementation of getAnalysisID() from
        // llvm/PassAnalysisSupport.h. Pass::getAnalysisID() is a template,
        // however it does not know a  type of required pass. So, copy body of
        // getAnalysisID() without type cast.
        assert(getResolver() &&
               "Pass has not been inserted into a PassManager object!");
        for (auto &ID : R[tsar::AnalysisRequest::AnalysisIDs]) {
          auto ResultPass = getResolver()->findImplPass(ID);
          assert(ResultPass && "getAnalysis*() called on an analysis that was "
                               "not 'required' by pass!");
          Response[tsar::AnalysisResponse::Analysis].push_back(
              ResultPass->getAdjustedAnalysisPointer(ID));
        }
      }
      return tsar::AnalysisSocket::Data +
             ::json::Parser<tsar::AnalysisResponse>::unparseAsObject(Response);
    }))
      ;
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
} // namespace llvm
#endif // TSAR_ANALYSIS_SERVER_H
