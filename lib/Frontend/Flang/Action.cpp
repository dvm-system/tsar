//===- Action.cpp ------ TSAR Frontend Action (Flang) ------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file contains front-end actions which are necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#include <flang/Optimizer/Support/InitFIR.h>
#include "tsar/Frontend/Flang/Action.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include <flang/Frontend/CompilerInstance.h>
#include <flang/Lower/Bridge.h>
#include <flang/Lower/Support/Verifier.h>
#include <flang/Optimizer/Support/Utils.h>
#include <mlir/IR/Dialect.h>
#include <mlir/Pass/PassManager.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Timer.h>

using namespace llvm;
using namespace tsar;
using namespace Fortran;

bool tsar::FlangMainAction::beginSourceFileAction() {
  using namespace Fortran::frontend;
  // CodeGenAction::BeginSourceFileAction() is private, so we cannot call
  // it explicitly here. We also want to extend it functionality here, so
  // we juast copy its implementation.

  llvmCtx = std::make_unique<llvm::LLVMContext>();

  // If the input is an LLVM file, just parse it and return.
  if (this->getCurrentInput().getKind().getLanguage() == Language::LLVM_IR)
    return false;

  // Otherwise, generate an MLIR module from the input Fortran source
  assert(getCurrentInput().getKind().getLanguage() == Language::Fortran &&
         "Invalid input type - expecting a Fortran file");
  bool res = runPrescan() && runParse() && runSemanticChecks();
  if (!res)
    return res;

  CompilerInstance &ci = this->getInstance();

  // Load the MLIR dialects required by Flang
  mlir::DialectRegistry registry;
  mlirCtx = std::make_unique<mlir::MLIRContext>(registry);
  fir::support::registerNonCodegenDialects(registry);
  fir::support::loadNonCodegenDialects(*mlirCtx);

  // Create a LoweringBridge
  const common::IntrinsicTypeDefaultKinds &defKinds =
      ci.getInvocation().getSemanticsContext().defaultKinds();
  fir::KindMapping kindMap(mlirCtx.get(),
      llvm::ArrayRef<fir::KindTy>{fir::fromDefaultKinds(defKinds)});
  lower::LoweringBridge lb = Fortran::lower::LoweringBridge::create(
      *mlirCtx, defKinds, ci.getInvocation().getSemanticsContext().intrinsics(),
      ci.getParsing().allCooked(), ci.getInvocation().getTargetOpts().triple,
      kindMap);

  // Create a parse tree and lower it to FIR
  Fortran::parser::Program &parseTree{*ci.getParsing().parseTree()};
  lb.lower(parseTree, ci.getInvocation().getSemanticsContext());
  mlirModule = std::make_unique<mlir::ModuleOp>(lb.getModule());

  // run the default passes.
  mlir::PassManager pm(mlirCtx.get(), mlir::OpPassManager::Nesting::Implicit);
  pm.enableVerifier(/*verifyPasses=*/true);
  pm.addPass(std::make_unique<Fortran::lower::VerifierPass>());

  if (mlir::failed(pm.run(*mlirModule))) {
    unsigned diagID = ci.getDiagnostics().getCustomDiagID(
        clang::DiagnosticsEngine::Error,
        "verification of lowering to FIR failed");
    ci.getDiagnostics().Report(diagID);
    return false;
  }
  // And of CodeGenAction::BeginSourceFileAction() copy.

  return mQueryManager.beginSourceFile(
      getInstance().getDiagnostics(), getCurrentFile(),
      getInstance().getFrontendOpts().outputFile, getWorkingDir());
}

bool tsar::FlangMainAction::shouldEraseOutputFiles() {
  mQueryManager.endSourceFile(
      getInstance().getDiagnostics().hasErrorOccurred());
  return Fortran::frontend::CodeGenAction::shouldEraseOutputFiles();
}

void tsar::FlangMainAction::executeAction() {
  auto &CI{getInstance()};
  generateLLVMIR();
  Timer LLVMIRAnalysis{"LLVMIRAnalysis", "LLVM IR Analysis Time"};
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.startTimer();
  auto CUs{llvmModule->getNamedMetadata("llvm.dbg.cu")};
  if (CUs->getNumOperands() == 1) {
    auto *CU{cast<DICompileUnit>(*CUs->op_begin())};
    IntrusiveRefCntPtr<TransformationContextBase> TfmCtx{
        new FlangTransformationContext{
            CI.getParsing(), CI.getInvocation().getFortranOpts(),
            CI.getSemantics().context(), *llvmModule, *CU}};
    mTfmInfo.setContext(*CU, std::move(TfmCtx));
  }
  mQueryManager.run(llvmModule.get(), &mTfmInfo);
  if (llvm::TimePassesIsEnabled)
    LLVMIRAnalysis.stopTimer();
}
