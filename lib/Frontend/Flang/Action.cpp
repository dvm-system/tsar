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
#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Frontend/Flang/Action.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Flang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include <flang/Frontend/CompilerInstance.h>
#include <flang/Lower/Bridge.h>
#include <flang/Lower/Support/Verifier.h>
#include <flang/Parser/parse-tree-visitor.h>
#include <flang/Semantics/scope.h>
#include <flang/Optimizer/Support/Utils.h>
#include <mlir/IR/Dialect.h>
#include <mlir/Pass/PassManager.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Timer.h>

using namespace llvm;
using namespace tsar;
using namespace Fortran;

bool tsar::FlangMainAction::beginSourceFileAction() {
  using namespace Fortran::frontend;
  if (this->getCurrentInput().getKind().getLanguage() == Language::LLVM_IR ||
      this->getCurrentInput().getKind().getLanguage() == Language::MLIR)
    return false;

  // CodeGenAction::beginSourceFileAction() is private, so we cannot call
  // it explicitly here. We also want to extend it functionality here, so
  // we have just copied its implementation.

  llvmCtx = std::make_unique<llvm::LLVMContext>();
  CompilerInstance &ci = this->getInstance();

  // Generate an MLIR module from the input Fortran source
  assert(getCurrentInput().getKind().getLanguage() == Language::Fortran &&
         "Invalid input type - expecting a Fortran file");
  bool res = runPrescan() && runParse() && runSemanticChecks() &&
             generateRtTypeTables();
  if (!res)
    return res;


  // Load the MLIR dialects required by Flang
  mlir::DialectRegistry registry;
  mlirCtx = std::make_unique<mlir::MLIRContext>(registry);
  fir::support::registerNonCodegenDialects(registry);
  fir::support::loadNonCodegenDialects(*mlirCtx);
  fir::support::loadDialects(*mlirCtx);
  fir::support::registerLLVMTranslation(*mlirCtx);

  // Create a LoweringBridge
  const common::IntrinsicTypeDefaultKinds &defKinds =
      ci.getInvocation().getSemanticsContext().defaultKinds();
  fir::KindMapping kindMap(mlirCtx.get(),
      llvm::ArrayRef<fir::KindTy>{fir::fromDefaultKinds(defKinds)});
  lower::LoweringBridge lb = Fortran::lower::LoweringBridge::create(
      *mlirCtx, defKinds, ci.getInvocation().getSemanticsContext().intrinsics(),
      ci.getInvocation().getSemanticsContext().targetCharacteristics(),
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

namespace {
struct DINodeReplacer {
  DINodeReplacer(DINode *From, DINode *To) : From(From), To(To) {}

  void visitMDNode(MDNode &MD) {
    if (!MDNodes.insert(&MD).second)
      return;
    for (unsigned I{0}, EI{MD.getNumOperands()}; I < EI; ++I) {
      auto &Op{MD.getOperand(I)};
      if (!Op.get())
        continue;
      if (Op == From)
        MD.replaceOperandWith(I, To);
      if (auto *N = dyn_cast<MDNode>(Op)) {
        visitMDNode(*N);
        continue;
      }
    }
  }

  SmallPtrSet<const Metadata *, 32> MDNodes;
  DINode *From;
  DINode *To;
};

class ProgramUnitCollector {
public:
  using LineToSubprogramMap =
      DenseMap<int, std::tuple<DISubprogram *, Function *>, DenseMapInfo<int>,
               TaggedDenseMapTuple<bcl::tagged<int, int>,
                                   bcl::tagged<DISubprogram *, DISubprogram>,
                                   bcl::tagged<Function *, Function>>>;

  explicit ProgramUnitCollector(const parser::AllCookedSources &AllCooked,
                                LineToSubprogramMap &LineToSubprogram,
                                LLVMContext &Ctx, DIScope &Scope)
      : mAllCooked(AllCooked), mLineToSubprogram(LineToSubprogram), mCtx(Ctx) {
    mScopeStack.push_back(&Scope);
  }

  template <typename T> bool Pre(T &N) { return true; }
  template <typename T> void Post(T &N) {}

  bool Pre(parser::ProgramUnit &PU) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this](common::Indirection<parser::MainProgram> &X) {
              if (const auto &S{std::get<
                      std::optional<parser::Statement<parser::ProgramStmt>>>(
                      X.value().t)})
                return preUnitStatement(*S);
              mInUnnamedProgram = true;
              return true;
            },
            [this](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            },
            [this](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            },
            [this](common::Indirection<parser::Module> &X) {
              const auto &S{
                  std::get<parser::Statement<parser::ModuleStmt>>(X.value().t)};
              const auto &Name{S.statement.v.symbol->name()};
              auto Range{mAllCooked.GetSourcePositionRange(S.source)};
              auto *Scope{mScopeStack.back()};
              auto DIM{DIModule::get(
                  Scope->getContext(), Scope->getFile(), Scope,
                  MDString::get(Scope->getContext(), Name.ToString()), nullptr,
                  nullptr, nullptr, Range ? Range->first.line : 0)};
              mScopeStack.push_back(DIM);
              return true;
            }},
        PU.u);
  }

  bool Pre(parser::InternalSubprogram &IS) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            },
            [this](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            }},
        IS.u);
  }

  bool Pre(parser::ModuleSubprogram &MS) {
    return std::visit(
        common::visitors{
            [](auto &) { return false; },
            [this](common::Indirection<parser::FunctionSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::FunctionStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            },
            [this](common::Indirection<parser::SubroutineSubprogram> &X) {
              const auto &S{std::get<parser::Statement<parser::SubroutineStmt>>(
                  X.value().t)};
              return preUnitStatement(S);
            }},
        MS.u);
  }

  void Post(parser::ProgramUnit &PU) { mScopeStack.pop_back(); }
  void Post(parser::InternalSubprogram &) { mScopeStack.pop_back(); }
  void Post(parser::ModuleSubprogram &) { mScopeStack.pop_back(); }

  bool Pre(parser::InternalSubprogramPart &ISP) {
    if (mInUnnamedProgram)
      mDeferredMainSubprograms = &ISP;
    return !mInUnnamedProgram;
  }

  template <typename T> bool Pre(parser::Statement<T> &S) {
    if (mInUnnamedProgram) {
      if (auto Range{mAllCooked.GetSourcePositionRange(S.source)})
        if (auto LToSItr{mLineToSubprogram.find(Range->first.line)};
            LToSItr != mLineToSubprogram.end() &&
            LToSItr->template get<DISubprogram>()) {
          markAsMain(LToSItr);
          LToSItr->template get<DISubprogram>()->replaceOperandWith(
              1, mScopeStack.back());
          assert(LToSItr->template get<DISubprogram>()->getScope() ==
                     mScopeStack.back() &&
                 "Corrupted metadata!");
          LToSItr->template get<DISubprogram>()->replaceOperandWith(
              2, MDString::get(mCtx,
                               FlangTransformationContext::UnnamedProgramStub));
          assert(LToSItr->template get<DISubprogram>()->getName() ==
                     FlangTransformationContext::UnnamedProgramStub &&
                 "Corrupted metadata!");
          mScopeStack.push_back(LToSItr->template get<DISubprogram>());
          mDIMainProgram = LToSItr->template get<DISubprogram>();
          // If we haven't processed internal subprograms in the main program
          // yet, it's not necessary to defer the processing, because we
          // already known DIScope for the main program.
          // If there is no executable constracts in the unnamed main program,
          // it's line in a metadata corresponds to the end statement, so
          // we have already deferred processing of internal subprograms.
          mInUnnamedProgram = false;
          return false;
        }
    }
    return true;
  }

  parser::InternalSubprogramPart * getDeferredSubprograms() noexcept {
    return mDeferredMainSubprograms;
  }

  DISubprogram * getDIMainProgram() noexcept { return mDIMainProgram; }

private:
  template <typename T> bool preUnitStatement(const T &S) {
    if (auto Range{mAllCooked.GetSourcePositionRange(S.source)})
      if (auto LToSItr{mLineToSubprogram.find(Range->first.line)};
          LToSItr != mLineToSubprogram.end() &&
          LToSItr->template get<DISubprogram>()) {
        LToSItr->template get<DISubprogram>()->replaceOperandWith(
            1, mScopeStack.back());
        assert(LToSItr->template get<DISubprogram>()->getScope() ==
                   mScopeStack.back() &&
               "Corrupted metadata!");
        if constexpr (std::is_same_v<T,
                                     parser::Statement<parser::ProgramStmt>>) {
          markAsMain(LToSItr);
          LToSItr->template get<DISubprogram>()->replaceOperandWith(
              2, MDString::get(mCtx, S.statement.v.symbol->name().ToString()));
          assert(LToSItr->template get<DISubprogram>()->getName() ==
                     S.statement.v.symbol->name().ToString() &&
                 "Corrupted metadata!");
          mDIMainProgram = LToSItr->template get<DISubprogram>();
        } else {
          const auto &Name{std::get<parser::Name>(S.statement.t)};
          LToSItr->template get<DISubprogram>()->replaceOperandWith(
              2, MDString::get(mCtx, Name.symbol->name().ToString()));
          assert(LToSItr->template get<DISubprogram>()->getName() ==
                     Name.symbol->name().ToString() &&
                 "Corrupted metadata!");
        }
        mScopeStack.push_back(LToSItr->template get<DISubprogram>());
        return true;
      }
    return false;
  }

  /// Set SPFlagMainSubprogram for main program unit.
  void markAsMain(const LineToSubprogramMap::iterator &LToSItr) {
    assert(LToSItr != mLineToSubprogram.end() && "Subprogram must be known!");
    auto NewDISub{DISubprogram::getDistinct(
        LToSItr->get<DISubprogram>()->getContext(),
        LToSItr->get<DISubprogram>()->getScope(),
        LToSItr->get<DISubprogram>()->getName(),
        LToSItr->get<DISubprogram>()->getLinkageName(),
        LToSItr->get<DISubprogram>()->getFile(),
        LToSItr->get<DISubprogram>()->getLine(),
        LToSItr->get<DISubprogram>()->getType(),
        LToSItr->get<DISubprogram>()->getScopeLine(),
        LToSItr->get<DISubprogram>()->getContainingType(),
        LToSItr->get<DISubprogram>()->getVirtualIndex(),
        LToSItr->get<DISubprogram>()->getThisAdjustment(),
        LToSItr->get<DISubprogram>()->getFlags(),
        LToSItr->get<DISubprogram>()->getSPFlags() |
            DISubprogram::SPFlagMainSubprogram,
        LToSItr->get<DISubprogram>()->getUnit(),
        LToSItr->get<DISubprogram>()->getTemplateParams(),
        LToSItr->get<DISubprogram>()->getDeclaration(),
        LToSItr->get<DISubprogram>()->getRetainedNodes(),
        LToSItr->get<DISubprogram>()->getThrownTypes(),
        LToSItr->get<DISubprogram>()->getAnnotations(),
        LToSItr->get<DISubprogram>()->getTargetFuncName())};
    DINodeReplacer R{LToSItr->get<DISubprogram>(), NewDISub};
    SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
    LToSItr->get<Function>()->getAllMetadata(MDs);
    for (auto &I : instructions(LToSItr->get<Function>())) {
      if (auto &Loc {I.getDebugLoc()})
        MDs.emplace_back(0, Loc.get());
      if (auto *MD{I.getMetadata(LLVMContext::MD_loop)})
        for (unsigned I = 1; I < MD->getNumOperands(); ++I)
          if (auto *Loc{dyn_cast_or_null<DILocation>(MD->getOperand(I))})
            MDs.emplace_back(0, Loc);
    }
    for (auto &MD : MDs)
      R.visitMDNode(*MD.second);
    LToSItr->get<Function>()->setSubprogram(NewDISub);
    LToSItr->get<DISubprogram>() = NewDISub;
  }

  const Fortran::parser::AllCookedSources &mAllCooked;
  LineToSubprogramMap &mLineToSubprogram;
  LLVMContext &mCtx;
  SmallVector<DIScope *, 4> mScopeStack;
  parser::InternalSubprogramPart *mDeferredMainSubprograms{nullptr};
  DISubprogram *mDIMainProgram{nullptr};
  bool mInUnnamedProgram{false};
};
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
    SmallString<128> CUFilePath;
    auto *DIF{CU->getFile()};
    if (DIF)
      getAbsolutePath(*CU, CUFilePath);
    if (!sys::fs::exists(CUFilePath) || !isFortran(CU->getSourceLanguage())) {
      auto Filename{getCurrentFile()};
      assert(sys::path::is_absolute(Filename) &&
             "Path to a processed file must be absolute!");
      SmallString<128> Directory{Filename};
      sys::path::remove_filename(Directory);
      auto *NewDIFile{
          DIFile::get(llvmModule->getContext(), Filename, Directory)};
      auto NewDICU{DICompileUnit::getDistinct(
          llvmModule->getContext(),
          isFortran(CU->getSourceLanguage()) ? CU->getSourceLanguage()
                                             : dwarf::DW_LANG_Fortran08,
          NewDIFile, CU->getProducer(), CU->isOptimized(), CU->getFlags(),
          CU->getRuntimeVersion(), CU->getSplitDebugFilename(),
          CU->getEmissionKind(), CU->getEnumTypes(), CU->getRetainedTypes(),
          CU->getGlobalVariables(), CU->getImportedEntities(), CU->getMacros(),
          CU->getDWOId(), CU->getSplitDebugInlining(),
          CU->getDebugInfoForProfiling(), CU->getNameTableKind(),
          CU->getRangesBaseAddress(), CU->getSysRoot(), CU->getSDK())};
      llvmModule->setSourceFileName(Filename);
      DINodeReplacer R{CU, NewDICU};
      for (auto &F : *llvmModule) {
        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        F.getAllMetadata(MDs);
        for (auto &MD : MDs)
          R.visitMDNode(*MD.second);
      }
      CUs->setOperand(0, NewDICU);
      CU = NewDICU;
    }
    ProgramUnitCollector::LineToSubprogramMap LineToSubprogram;
    for (auto &F:*llvmModule) {
      if (auto *DISub{F.getSubprogram()}) {
        auto [I, IsNew] =
            LineToSubprogram.try_emplace(DISub->getLine(), DISub, &F);
        if (!IsNew) {
          I->get<DISubprogram>() = nullptr;
        }
      }
    }
    ProgramUnitCollector V{CI.getParsing().allCooked(), LineToSubprogram,
                           llvmModule->getContext(), *CU};
    parser::Walk(CI.getParsing().parseTree(), V);
    if (V.getDeferredSubprograms() && V.getDIMainProgram()) {
      ProgramUnitCollector DeferredV{CI.getParsing().allCooked(),
                                     LineToSubprogram, llvmModule->getContext(),
                                     *V.getDIMainProgram()};
      parser::Walk(*V.getDeferredSubprograms(), DeferredV);
    }
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
