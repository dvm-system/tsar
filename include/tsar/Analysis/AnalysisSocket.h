//===--- AnalysisSocket.h ------ Analysis Socket ----------------*- C++ -*-===//
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

#ifndef TSAR_ANALYSIS_SOCKET_H
#define TSAR_ANALYSIS_SOCKET_H

#include "tsar/Analysis/AnalysisWrapperPass.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/Socket.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Pass.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>

namespace tsar {
/// This class allows to establish connection to analysis server and to obtain
/// analysis results and perform synchronization between a client and a server.
class AnalysisSocket final : public bcl::Socket<std::string> {
public:
  static constexpr const char Delimiter = '$';
  static constexpr const char Space = ' ';
  static constexpr const char *Wait = "wait";

  ~AnalysisSocket() noexcept {
    // Avoid exception in destructor.
    try {
      close();
    } catch (...) {
      llvm::report_fatal_error("unable to close connection with server");
    }
  }

  /// Close connection.
  void close() {
    for (auto &Callback : mClosedCallbacks)
      Callback(false);
    mClosedCallbacks.clear();
    mReceiveCallbacks.clear();
  }

  /// Send response from a server to client.
  ///
  /// Response is a string representation of an address which points to an
  /// analysis pass. A `nullptr` could be encoded with empty string.
  ///
  /// Note, that physically this method runs in the client to set response
  /// representation.
  void send(const std::string &Response) const override {
    assert(Response.back() == Delimiter && "Last character must be a delimiter!");
    llvm::StringRef RawPtr(Response.data(), Response.size() - 1);
    llvm::Pass *P = nullptr;
    if (!RawPtr.empty()) {
      std::uintptr_t IntToPtr;
      llvm::to_integer(RawPtr, IntToPtr, 10);
      P = reinterpret_cast<llvm::Pass*>(IntToPtr);
    }
    mAnalysisPass = P;
  }

  /// Register a callback which is invoked whenever a server receive data.
  void receive(const ReceiveCallback &F) const override {
    mReceiveCallbacks.push_back(F);
  }

  /// Register a callback which is invoked on a server if corresponding
  /// connection is closed.
  void closed(const ClosedCallback &F) const override {
    mClosedCallbacks.push_back(F);
  }

  /// Retrieve a specified analysis results from a server.
  template<class AnalysisType> AnalysisType * getAnalysis() {
    std::string RawID;
    llvm::raw_string_ostream OS(RawID);
    OS << reinterpret_cast<uintptr_t>(&AnalysisType::ID) << Delimiter;
    for (auto &Callback : mReceiveCallbacks)
      Callback(OS.str());
    // Note, that callback run send() in client, so mAnalysisPass is already
    // set here.
    return static_cast<AnalysisType *>(mAnalysisPass);
  }

  /// Retrieve a specified analysis results from a server.
  template<class AnalysisType> AnalysisType * getAnalysis(llvm::Function &F) {
    std::string RawID;
    llvm::raw_string_ostream OS(RawID);
    OS << reinterpret_cast<uintptr_t>(&AnalysisType::ID) << Space
       << reinterpret_cast<uintptr_t>(&F) << Delimiter;
    for (auto &Callback : mReceiveCallbacks)
      Callback(OS.str());
    // Note, that callback run send() in client, so mAnalysisPass is already
    // set here.
    return static_cast<AnalysisType *>(mAnalysisPass);
  }

  /// Wait notification from a server.
  void wait() {
    do {
      for (auto &Callback : mReceiveCallbacks)
        Callback(Wait);
        // Note, that callback run send() in client, so mAnalysisPass is already
        // set here.
    } while (mAnalysisPass !=
             llvm::DenseMapInfo<llvm::Pass *>::getTombstoneKey());
  }

private:
  mutable llvm::SmallVector<ReceiveCallback, 1> mReceiveCallbacks;
  mutable llvm::SmallVector<ClosedCallback, 1> mClosedCallbacks;
  mutable llvm::Pass *mAnalysisPass;
};
}

namespace llvm {
/// Wrapper to allow client passes access analysis socket.
using AnalysisSocketImmutableWrapper =
  AnalysisWrapperPass<tsar::AnalysisSocket>;

/// Wrapper to allow server passes access analysis connection.
///
/// Note, that it should be explicitly initialized in 'run' function which
/// is invoked when connection is established.
using AnalysisConnectionImmutableWrapper =
  AnalysisWrapperPass<bcl::IntrusiveConnection>;
}
#endif// TSAR_ANALYSIS_SOCKET_H
