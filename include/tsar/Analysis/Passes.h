//===- Passes.h - Create and Initialize Analysis Passes (Base) --*- C++ -*-===//
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
// It contains declarations of functions that initialize and create an instances
// of general TSAR analysis passes. Declarations of appropriate methods for an
// each new pass should be added to this file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_PASSES_H
#define TSAR_ANALYSIS_PASSES_H

namespace bcl {
class IntrusiveConnection;
}

namespace llvm {
class PassRegistry;
class PassInfo;
class ImmutablePass;
class FunctionPass;
class ModulePass;
class raw_ostream;

/// Initialize base analysis passes.
void initializeAnalysisBase(PassRegistry &Registry);

/// Create a pass to print internal state of the specified pass after the
/// last execution.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
FunctionPass * createFunctionPassPrinter(const PassInfo *PI, raw_ostream &OS);

/// Create a pass to print internal state of the specified pass after the
/// last execution.
///
/// To use this function it is necessary to override
/// void `llvm::Pass::print(raw_ostream &O, const Module *M) const` method for
/// a function pass internal state of which must be printed.
ModulePass * createModulePassPrinter(const PassInfo *PI, raw_ostream &OS);

/// Initialize a pass to build hierarchy of data-flow regions.
void initializeDFRegionInfoPassPass(PassRegistry &Registry);

/// Create a pass to build hierarchy of data-flow regions.
FunctionPass * createDFRegionInfoPass();

/// Initialize a wrapper to access analysis socket.
void initializeAnalysisSocketImmutableWrapperPass(PassRegistry &Registry);

/// Initialize immutable storage for analysis socket.
void initializeAnalysisSocketImmutableStoragePass(PassRegistry &Registry);

/// Create immutable storage for analysis socket.
ImmutablePass *createAnalysisSocketImmutableStorage();

/// Initialize immutable pass to access analysis connection.
void initializeAnalysisConnectionImmutableWrapperPass(PassRegistry& Registry);

/// Create immutable pass to access analysis connection.
ImmutablePass *createAnalysisConnectionImmutableWrapper(
  bcl::IntrusiveConnection &C);

/// Initialize a pass to notify client as soon as server receives 'wait' request.
void initializeAnalysisNotifyClientPassPass(PassRegistry &Registry);

/// Notify client as soon as server receives 'wait' request.
ModulePass * createAnalysisNotifyClientPass();

/// Initialize a pass to notify server that all requests have been processed
/// and it may execute further passes.
void initializeAnalysisReleaseServerPassPass(PassRegistry &Registry);

/// Notify server that all requests have been processed and it may execute
/// further passes.
ModulePass * createAnalysisReleaseServerPass(bool ActiveOnly = false);

/// Notify server that all requests have been processed and it may execute
/// further passes.
ModulePass * createAnalysisReleaseServerPass(const void * ServerID);

/// Initialize a pass to block client until server send notification.
void initializeAnalysisWaitServerPassPass(PassRegistry &Registry);

/// Block client until server sends notification.
ModulePass * createAnalysisWaitServerPass(bool ActiveOnly = false);

/// Block client until server sends notification.
ModulePass * createAnalysisWaitServerPass(const void * ServerID);

/// Initialize a pass to close connection with server.
void initializeAnalysisCloseConnectionPassPass(PassRegistry &Registry);

/// Close connection with server (it should be run on client). Client will
/// be blocked until server confirms that connection can be closed.
ModulePass *createAnalysisCloseConnectionPass(bool ActiveOnly = false);

/// Close connection with server (it should be run on client). Client will
/// be blocked until server confirms that connection can be closed.
ModulePass *createAnalysisCloseConnectionPass(const void * ServerID);
}
#endif//TSAR_ANALYSIS_PASSES_H
