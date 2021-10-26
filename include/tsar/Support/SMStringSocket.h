//===--- SMStringSocket.h ------ Analysis Socket ----------------*- C++ -*-===//
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

#ifndef TSAR_SUPPORT_SM_STRING_SOCKET_H
#define TSAR_SUPPORT_SM_STRING_SOCKET_H

#include <bcl/Socket.h>
#include <llvm/Support/ErrorHandling.h>

namespace tsar {
/// This class allows to establish connection to a server which must be run as
/// a separate thread.
///
/// Response from server must be a string '<MessageKind> + <response data>' and
/// SocketImpl class can implement processResponse() method to process a
/// response from the server.
template<typename SocketImpl>
class SMStringSocketBase : public bcl::Socket<std::string> {
public:
  enum MessageKind : char {
    Delimiter = '$',
    Wait = 'w',
    Release = 'r',
    Notify = 'n',
    Data = 'd',
    Invalid = 'i',
  };

  friend inline bool operator==(const MessageTy &R, MessageKind K) {
    return R.size() == 1 && R.front() == K;
  }
  friend inline bool operator==(MessageKind K, const MessageTy &R) {
    return operator==(R, K);
  }
  friend inline bool operator!=(const MessageTy &R, MessageKind K) {
    return !operator==(R, K);
  }
  friend inline bool operator!=(MessageKind K, const MessageTy &R) {
    return !operator==(R, K);
  }

  friend inline std::string operator+(const MessageTy &R, MessageKind K) {
    return R + std::string({ K });
  }
  friend inline std::string operator+(MessageKind K, const MessageTy &R) {
    return std::string({ K }) + R;
  }

  ~SMStringSocketBase() noexcept {
    // Avoid exception in destructor.
    try {
      close();
    }
    catch (...) {
      llvm::report_fatal_error("unable to close connection with server");
    }
  }

  /// Send response from a server to client.
  ///
  /// It must be a string '<MessageKind> + <response data>'.
  /// Note, that physically this method runs in the client to set response
  /// representation.
  void send(const std::string &Response) const override {
    assert(Response.back() == Delimiter &&
      "Last character must be a delimiter!");
    mResponseKind = static_cast<MessageKind>(Response.front());
    if (mResponseKind == Data)
      static_cast<const SocketImpl *>(this)->processResponse(Response);
  }

  /// Just do nothing, if necessary a derived class should implement this
  /// method to process a response from server the server.
  void processRespone(const std::string &Response) const {}

  /// Close connection.
  void close() {
    for (auto &Callback : mClosedCallbacks)
      Callback(false);
    mClosedCallbacks.clear();
    mReceiveCallbacks.clear();
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

  /// Wait notification from a server.
  void wait() {
    do {
      for (auto &Callback : mReceiveCallbacks)
        Callback({ Wait });
      // Note, that callback run send() in client, so response data is already
      // set here.
    } while (mResponseKind != Notify);
  }

  /// Notify server that all requests have been processed and it may go
  /// further.
  void release() {
    do {
      for (auto &Callback : mReceiveCallbacks)
        Callback({ Release });
      // Note, that callback run send() in client, so response data is already
      // set here.
    } while (mResponseKind != Notify);
  }

protected:
  mutable llvm::SmallVector<ReceiveCallback, 1> mReceiveCallbacks;
  mutable llvm::SmallVector<ClosedCallback, 1> mClosedCallbacks;
  mutable MessageKind mResponseKind;
};
}
#endif//TSAR_SUPPORT_SM_STRING_SOCKET_H
