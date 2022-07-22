//=== DIClientServerInfo.h - Client Server Analysis Matcher -----*- C++ -*-===//
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
// This file defines is a helpful class to obtain analysis results from server.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_CLIENT_SERVER_INFO_H
#define TSAR_DI_CLIENT_SERVER_INFO_H

#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include <tsar/Support/Tags.h>
#include <bcl/tagged.h>
#include <functional>

namespace llvm {
class DataLayout;
class DominatorTree;
class Function;
class Loop;
class Pass;
class Value;
}

namespace tsar {
class EstimateMemory;
class AnalysisSocket;

/// This is a helpful class to obtain analysis results from active server.
///
/// If server is available this class fetches necessary information from server.
/// Otherwise, this class is a simple wrapper for analysis on client and
/// it returns information from client.
struct DIClientServerInfo {
  DIClientServerInfo(llvm::Pass &P, llvm::Function &F,
                     DIAliasTree *ClientDIAT = nullptr,
                     DIDependencInfo *ClientDIDepInfo = nullptr);

  DIClientServerInfo(AnalysisSocket *Socket, llvm::Function &F,
                     DIAliasTree *ClientDIAT = nullptr,
                     DIDependencInfo *ClientDIDepInfo = nullptr);

  /// Return true if data is available.
  bool isValid() const noexcept {
    return DIAT && DIDepInfo;
  }

  bool isValidMemory() const noexcept { return DIAT; }
  bool isValideDependecies() const noexcept { return DIDepInfo; }

  /// Return true if data is available.
  operator bool () const noexcept { return isValid(); }

  std::function<ObjectID(ObjectID)> getObjectID = [](ObjectID ID) {
    return ID;
  };
  std::function<llvm::Value *(llvm::Value *)> getValue = [](llvm::Value *V) {
    return V;
  };
  std::function<llvm::Metadata *(llvm::Metadata *)> getMetadata =
      [](llvm::Metadata *MD) {
    return MD;
  };
  std::function<DIMemory *(DIMemory *)> getMemory = [](DIMemory *M) {
    return M;
  };
  std::function<DIMemory *(DIMemory *)> getClientMemory = [](DIMemory *M) {
    return M;
  };

  /// Alias tree on server or on client or nullptr.
  DIAliasTree *DIAT = nullptr;
  /// Dependence analysis results on server on client or nullptr.
  DIDependencInfo *DIDepInfo = nullptr;
};

/// This is a helpful class to obtain analysis results from active server.
///
/// If server is available this class fetches necessary information from server.
/// Otherwise, this class is a simple wrapper for analysis on client and
/// it returns information from client.
struct DIMemoryClientServerInfo : public DIClientServerInfo {
  /// Obtain analysis from client and server.
  DIMemoryClientServerInfo(DIAliasTree &ClientDIAT, llvm::Pass &P,
    llvm::Function &F);

  /// Obtain analysis from client and server.
  DIMemoryClientServerInfo(DIAliasTree &ClientDIAT, AnalysisSocket *Socket,
    llvm::Function &F, DIDependencInfo *ClientDIDepInfo = nullptr);

  /// Return client-to-server memory pair for a specified estimate memory on
  /// client.
  ///
  /// \return Both values will be the same if server is not available.
  bcl::tagged_pair<bcl::tagged<DIMemory *, Origin>,
                   bcl::tagged<DIMemory *, Clone>>
  findFromClient(const EstimateMemory &EM, const llvm::DataLayout &DL,
                 llvm::DominatorTree &DT) const;

  /// Return client-to-server memory pair for a specified value on client.
  ///
  /// \return Both values will be the same if server is not available.
  bcl::tagged_pair<bcl::tagged<DIMemory *, Origin>,
                   bcl::tagged<DIMemory *, Clone>>
  findFromClient(llvm::Value &V, llvm::DominatorTree &DT,
                 DIUnknownMemory::Flags F) const;

  /// Return analysis results from server for a specified loop on client.
  ///
  /// If server is not available and analysis results could are available on
  /// client, this function returns analysis results from client.
  DIDependenceSet *findFromClient(const llvm::Loop &L) const;

  /// Return true if data is available.
  operator bool () const noexcept { return isValid(); }

  /// Return true if analysis results available from server.
  ///
  /// If not, they may be still available from client.
  bool isServerAvailable() const noexcept {
    return isValid() && ClientDIAT != DIAT;
  }

  DIAliasTree *ClientDIAT = nullptr;
};
}

#endif//TSAR_DI_CLIENT_SERVER_INFO_H
