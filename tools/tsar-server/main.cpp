//===-- main.cpp --- Traits Static Analyzer (Network Server) ---*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Groupv
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
// This file implements a network server which is listening for a single
// connection and runs Traits Static AnalyzeR (TSAR) if connection is
// successfully established.
//
// The server accepts a single command line parameter which is a name of log
// file. If it is specified, the server writes description of any network event
// to this file.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/tsar-config.h"
#include <bcl/CSocket.h>
#include <bcl/Json.h>
#include <llvm/ADT/StringRef.h>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <fstream>
#include <string>

namespace tsar {
namespace net {
JSON_OBJECT_BEGIN(Socket)
JSON_OBJECT_ROOT_PAIR_8(Socket
  , TSARVersion, std::string
  , Date, std::string
  , Status, std::string
  , ServerAddress, std::string
  , ServerPort, int
  , ClientAddress, std::string
  , ClientPort, int
  , Message, std::string
)
  Socket() : JSON_INIT_ROOT {}
JSON_OBJECT_END(Socket)
}
}
JSON_DEFAULT_TRAITS(tsar::net::, Socket)

using namespace tsar;

static inline void setCurrentDate(net::Socket &Status) {
  auto T = std::chrono::system_clock::now();
  auto CTime = std::chrono::system_clock::to_time_t(T);
  char TimeBuffer[100];
  if (std::strftime(TimeBuffer, sizeof(TimeBuffer),
       "%F %T", std::localtime(&CTime)))
    Status[net::Socket::Date] = TimeBuffer;
  else
    Status[net::Socket::Date] = "";
}

namespace {
class Log {
public:
  Log() {}

  ~Log() {
    mLogFile << "\n  ]\n}\n";
    mLogFile.flush();
  }

  inline void init(llvm::StringRef App, llvm::StringRef Date,
      llvm::StringRef Version, std::fstream &&LogFile) {
    mLogFile = std::move(LogFile);
    mLogFile << "{\n  " << R"("Application": )" << '"' << App.data() << '"';
    mLogFile << ",\n  " << R"("Date": )" << '"' << Date.data() << '"';
    mLogFile << ",\n  " << R"("Version": )" << '"' << Version.data() << '"';
  }

  inline void print(net::Socket &Status, llvm::StringRef Prefix) {
    auto Tmp = json::Parser<net::Socket>::unparseAsObject(Status);
    mLogFile << Prefix.data() << Tmp;
    std::cout << Tmp << "\n";
    // Flush each line to allow listener to process each line of the output
    // as soon as it becomes available.
    std::cout.flush();
  }

private:
  std::fstream mLogFile;
} L;
}

static void sigHandler(int sig) {
  std::exit(0);
}

int main(int Argc, char **Argv) {
  std::signal(SIGINT, sigHandler);
  std::signal(SIGTERM, sigHandler);
  std::signal(SIGABRT, sigHandler);
  std::signal(SIGSEGV, sigHandler);
  std::signal(SIGILL, sigHandler);
  std::signal(SIGFPE, sigHandler);
  std::fstream LogFile;
  if (Argc == 2) {
    std::string Arg(Argv[1]);
    if (Arg == "-help" || Arg == "--help" || Arg == "-h") {
      std::cout << "USAGE: " << Argv[0] << " [<log filename>]\n";
      return 0;
    }
    if (Arg == "-version" || Arg == "--version" || Arg == "-v") {
      std::cout << "VERSION: " << Argv[0] << " " << TSAR_VERSION_STRING << "\n";
      return 0;
    }
    LogFile.open(Argv[1], std::ios::out);
    if (LogFile.fail()) {
      std::cerr << Argv[0] << ": error: unable to open log file";
      auto Error = std::strerror(errno);
      if (Error && strlen(Error) > 0)
        std::cerr << Error << "\n";
      return 1;
    }
  } else if (Argc > 2) {
    std::cerr << Argv[0] << ": error: too many arguments have benn specified\n";
    std::cerr << "USAGE: " << Argv[0] << " [<log filename>]\n";
    return 1;
  }
  net::Socket Status;
  setCurrentDate(Status);
  Status[net::Socket::TSARVersion] = TSAR_VERSION_STRING;
  Status[net::Socket::Status] = "start";
  Status[net::Socket::Message] = "start server";
  Status[net::Socket::ServerPort] = 0;
  Status[net::Socket::ClientPort] = 0;
  L.init(Argv[0], Status[net::Socket::Date], Status[net::Socket::TSARVersion],
    std::move(LogFile));
  L.print(Status, ",\n  \"Status\": [\n    ");
  bcl::net::startServer("localhost", 0, 0,
    [](bcl::net::SocketStatus StatusID, const bcl::net::Connection &Connection){
      net::Socket Status;
      setCurrentDate(Status);
      Status[net::Socket::TSARVersion] = TSAR_VERSION_STRING;
      Status[net::Socket::ServerAddress] = Connection.getServerAddress();
      Status[net::Socket::ServerPort] = Connection.getServerPort();
      if (Connection.isActive()) {
        Status[net::Socket::ClientAddress] = Connection.getClientAddress();
        Status[net::Socket::ClientPort] = Connection.getClientPort();
      } else {
        Status[net::Socket::ClientPort] = 0;
      }
      switch(StatusID) {
        case bcl::net::SocketStatus::Listen:
          Status[net::Socket::Status] = "listen";
          Status[net::Socket::Message] = "start listening for connection";
          break;
        case bcl::net::SocketStatus::Accept:
          Status[net::Socket::Status] = "accept";
          Status[net::Socket::Message] = "connection is established";
          break;
        case bcl::net::SocketStatus::Send:
          Status[net::Socket::Status] = "send";
          Status[net::Socket::Message] = "data is sent";
          break;
        case bcl::net::SocketStatus::Receive:
          Status[net::Socket::Status] = "receive";
          Status[net::Socket::Message] = "data is received";
          break;
        case bcl::net::SocketStatus::Close:
          Status[net::Socket::Status] = "close";
          Status[net::Socket::Message] = "socket is closed";
          break;
        case bcl::net::SocketStatus::InitializeError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to initialize socket library";
          break;
        case bcl::net::SocketStatus::CreateError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to create socket";
          break;
        case bcl::net::SocketStatus::BindError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to bind socket";
          break;
        case bcl::net::SocketStatus::ListenError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to listen for connection";
          break;
        case bcl::net::SocketStatus::AcceptError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to establish connection";
          break;
        case bcl::net::SocketStatus::SendError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to send data";
          break;
        case bcl::net::SocketStatus::ReceiveError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to receive data";
          break;
        case bcl::net::SocketStatus::HostnameError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to resolve hostname";
          break;
        case bcl::net::SocketStatus::ServerAddressError:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "unable to get address of the server";
          break;
        default:
          Status[net::Socket::Status] = "error";
          Status[net::Socket::Message] = "uknown error";
          break;
      }
      L.print(Status, ",\n    ");
    });
  return 0;
}
