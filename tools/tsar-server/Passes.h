//===------ Passes.h ---- Initialize TSAR Server Passes ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
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
// This contains functions to initialize passes which are necessary to use
// TSAR server.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SERVER_PASSES_H
#define TSAR_SERVER_PASSES_H

namespace bcl {
class IntrusiveConnection;
class RedirectIO;
}

namespace llvm {
class ModulePass;
class PassRegistry;

/// Create an interaction pass to obtain results of private variables analysis.
ModulePass * createPrivateServerPass(
  bcl::IntrusiveConnection &IC, bcl::RedirectIO &StdErr);

/// Initialize an interaction pass to obtain results of private variables
/// analysis.
void initializePrivateServerPassPass(PassRegistry &Registry);
}

#endif//TSAR_SERVER_PASSES_H
