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

#include "tsar/Support/AnalysisWrapperPass.h"
#include <bcl/cell.h>
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <bcl/Socket.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Pass.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Support/raw_ostream.h>

namespace tsar {
JSON_OBJECT_BEGIN(AnalysisRequest)
  JSON_OBJECT_ROOT_PAIR_2(AnalysisRequest,
    AnalysisIDs, std::vector<llvm::AnalysisID>,
    Function, llvm::Function *)
  AnalysisRequest() : JSON_INIT_ROOT {}
JSON_OBJECT_END(AnalysisRequest)

JSON_OBJECT_BEGIN(AnalysisResponse)
  JSON_OBJECT_ROOT_PAIR(AnalysisResponse, Analysis, std::vector<void *>)
  AnalysisResponse() : JSON_INIT_ROOT {}
JSON_OBJECT_END(AnalysisResponse)

/// This class allows to establish connection to analysis server and to obtain
/// analysis results and perform synchronization between a client and a server.
class AnalysisSocket final : public bcl::Socket<std::string> {
  /// Add requested analysis to the end of request.
  struct PushBackAnalysisID {
    template <class AnalysisType> void operator()() {
      Request[AnalysisRequest::AnalysisIDs].push_back(&AnalysisType::ID);
    }
    AnalysisRequest &Request;
  };

  /// Copy analysis to a map `Result`.
  template<class ResultT>
  struct InsertAnalysis {
    template <class AnalysisType> void operator()() {
      Result.template value<AnalysisType *>() =
          static_cast<AnalysisType *>(Analysis[Idx++]);

    }
    std::size_t &Idx;
    std::vector<void *> &Analysis;
    ResultT &Result;
  };

public:
  static constexpr const char Delimiter = '$';
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
    llvm::StringRef Json(Response.data(), Response.size() - 1);
    json::Parser<AnalysisResponse> Parser(Json);
    AnalysisResponse R;
    if (!Parser.parse(R))
      mAnalysis.clear();
    else
      mAnalysis = std::move(R[AnalysisResponse::Analysis]);
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
  template<class... AnalysisType>
  llvm::Optional<
    bcl::StaticTypeMap<typename std::add_pointer<AnalysisType>::type...>>
  getAnalysis() {
    using ResultT =
        bcl::StaticTypeMap<typename std::add_pointer<AnalysisType>::type...>;
    AnalysisRequest R;
    R[AnalysisRequest::Function] = nullptr;
    bcl::TypeList<AnalysisType...>::for_each_type(PushBackAnalysisID{R});
    auto Request =
        json::Parser<AnalysisRequest>::unparseAsObject(R) + Delimiter;
    for (auto &Callback : mReceiveCallbacks)
      Callback(Request);
    // Note, that callback run send() in client, so mAnalysisPass is already
    // set here.
    if (mAnalysis.size() == sizeof...(AnalysisType)) {
      ResultT Result;
      std::size_t Idx = 0;
      bcl::TypeList<AnalysisType...>::for_each_type(
          InsertAnalysis<ResultT>{Idx, mAnalysis, Result});
      return Result;
    }
    return llvm::None;
  }

  /// Retrieve a specified analysis results from a server.
  template<class... AnalysisType>
  llvm::Optional<
    bcl::StaticTypeMap<typename std::add_pointer<AnalysisType>::type...>>
  getAnalysis(llvm::Function &F) {
    using ResultT =
        bcl::StaticTypeMap<typename std::add_pointer<AnalysisType>::type...>;
    AnalysisRequest R;
    R[AnalysisRequest::Function] = &F;
    bcl::TypeList<AnalysisType...>::for_each_type(PushBackAnalysisID{R});
    auto Request =
        json::Parser<AnalysisRequest>::unparseAsObject(R) + Delimiter;
    for (auto &Callback : mReceiveCallbacks)
      Callback(Request);
    // Note, that callback run send() in client, so mAnalysisPass is already
    // set here.
    if (mAnalysis.size() == sizeof...(AnalysisType)) {
      ResultT Result;
      std::size_t Idx = 0;
      bcl::TypeList<AnalysisType...>::for_each_type(
          InsertAnalysis<ResultT>{Idx, mAnalysis, Result});
      return Result;
    }
    return llvm::None;
  }

  /// Wait notification from a server.
  void wait() {
    do {
      for (auto &Callback : mReceiveCallbacks)
        Callback(Wait);
        // Note, that callback run send() in client, so mAnalysisPass is already
        // set here.
    } while (mAnalysis.empty() && mAnalysis.front() !=
             llvm::DenseMapInfo<llvm::Pass *>::getTombstoneKey());
  }

private:
  mutable llvm::SmallVector<ReceiveCallback, 1> mReceiveCallbacks;
  mutable llvm::SmallVector<ClosedCallback, 1> mClosedCallbacks;
  mutable std::vector<void *> mAnalysis;
};
}

namespace json {
template <> struct CellTraits<tsar::json_::AnalysisRequestImpl::Function> {
  using CellKey = tsar::json_::AnalysisRequestImpl::Function;
  using ValueType = CellKey::ValueType;
  inline static bool parse(ValueType &Dest, Lexer &Lex)
      noexcept(
        noexcept(Traits<ValueType>::parse(Dest, Lex))) {
    uintptr_t RawDest;
    auto Res = Traits<uintptr_t>::parse(RawDest, Lex);
    if (Res)
      Dest = reinterpret_cast<llvm::Function *>(RawDest);
    return Res;
  }
  inline static void unparse(String &JSON, const ValueType &Obj)
      noexcept(
        noexcept(Traits<ValueType>::unparse(JSON, Obj))) {
    Traits<uintptr_t>::unparse(JSON, reinterpret_cast<uintptr_t>(Obj));
  }
  inline static typename std::result_of<
    decltype(&CellKey::name)()>::type name()
      noexcept(noexcept(CellKey::name())) {
    return CellKey::name();
  }
};

template <> struct CellTraits<tsar::json_::AnalysisRequestImpl::AnalysisIDs> {
  using CellKey = tsar::json_::AnalysisRequestImpl::AnalysisIDs;
  using ValueType = CellKey::ValueType;
  inline static bool parse(ValueType &Dest, Lexer &Lex) {
    std::vector<uintptr_t> RawDest;
    auto Res = Traits<std::vector<uintptr_t>>::parse(RawDest, Lex);
    if (Res) {
      Dest.clear();
      std::transform(RawDest.begin(), RawDest.end(), std::back_inserter(Dest),
                     [](uintptr_t RawPtr) {
                       return reinterpret_cast<llvm::AnalysisID>(RawPtr);
                     });
    }
    return Res;
  }
  inline static void unparse(String &JSON, const ValueType &Obj) {
    std::vector<uintptr_t> RawObj;
    std::transform(
        Obj.begin(), Obj.end(), std::back_inserter(RawObj),
        [](llvm::AnalysisID ID) { return reinterpret_cast<uintptr_t>(ID); });
    Traits<std::vector<uintptr_t>>::unparse(JSON, RawObj);
  }
  inline static typename std::result_of<
    decltype(&CellKey::name)()>::type name()
      noexcept(noexcept(CellKey::name())) {
    return CellKey::name();
  }
};

template <> struct CellTraits<tsar::json_::AnalysisResponseImpl::Analysis> {
  using CellKey = tsar::json_::AnalysisResponseImpl::Analysis;
  using ValueType = CellKey::ValueType;
  inline static bool parse(ValueType &Dest, Lexer &Lex) {
    std::vector<uintptr_t> RawDest;
    auto Res = Traits<std::vector<uintptr_t>>::parse(RawDest, Lex);
    if (Res) {
      Dest.clear();
      std::transform(RawDest.begin(), RawDest.end(), std::back_inserter(Dest),
                     [](uintptr_t RawPtr) {
                       return reinterpret_cast<void *>(RawPtr);
                     });
    }
    return Res;
  }
  inline static void unparse(String &JSON, const ValueType &Obj) {
    std::vector<uintptr_t> RawObj;
    std::transform(
        Obj.begin(), Obj.end(), std::back_inserter(RawObj),
        [](void *P) { return reinterpret_cast<uintptr_t>(P); });
    Traits<std::vector<uintptr_t>>::unparse(JSON, RawObj);
  }
  inline static typename std::result_of<
    decltype(&CellKey::name)()>::type name()
      noexcept(noexcept(CellKey::name())) {
    return CellKey::name();
  }
};
}

JSON_DEFAULT_TRAITS(tsar::, AnalysisRequest)
JSON_DEFAULT_TRAITS(tsar::, AnalysisResponse)

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
