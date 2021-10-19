//===- ClangDVMHWriter.cpp -- DVMH Program Generator ------------*- C++ -*-===//
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
// This file implements a pass to generate a DVMH program according to
// parallel variant obtained on previous steps of parallelization.
//
//===----------------------------------------------------------------------===//

#include "AstWrapperImpl.h"
#include "DistributionUtils.h"
#include "MemoryServer.h"
#include "tsar/APC/Passes.h"
#include "tsar/APC/APCContext.h"
#include "tsar/Analysis/Clang/GlobalInfoExtractor.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Frontend/Clang/ASTImportInfo.h"
#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/PassProvider.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/Passes.h"
#include <apc/Distribution/DvmhDirective.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <bcl/utility.h>
#include <clang/AST/Decl.h>
#include <clang/AST/ASTContext.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/ScalarEvolutionAliasAnalysis.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-experimental-apc-dvmh"

using namespace clang;
using namespace llvm;
using namespace tsar;
using namespace tsar::dvmh;

namespace {
/// Collect declaration traits.
class DeclarationInfoExtractor :
  public RecursiveASTVisitor<DeclarationInfoExtractor> {
public:
  /// Description of a declaration.
  struct DeclarationInfo {
    /// If set to `false` then a declaration statement for an appropriate
    /// declaration contains multiple declarations (for example, `int X, Y`).
    bool IsSingleDeclStmt = false;
  };

  /// Map from declaration to its traits.
  using DeclarationInfoMap = DenseMap<unsigned, DeclarationInfo>;

  explicit DeclarationInfoExtractor(DeclarationInfoMap &Decls) :
    mDecls(Decls) {}

  bool VisitDeclStmt(DeclStmt *DS) {
    for (auto *D : DS->decls())
      if (isa<VarDecl>(D)) {
        auto Loc = D->getLocation();
        mDecls[Loc.getRawEncoding()].IsSingleDeclStmt = DS->isSingleDecl();
      }
    return true;
  }

private:
  DeclarationInfoMap &mDecls;
};

class APCClangDVMHWriter : public ModulePass, private bcl::Uncopyable {
  /// Description of a template which is necessary for source-to-source
  /// transformation.
  struct TemplateInfo {
    /// If set to `false` then no definitions of a template exists in a source
    /// code. Note, that declarations with `extern` specification may exist.
    bool HasDefinition = false;
  };

  /// Contains templates which are used in program files.
  using TemplateInFileUsage =
    DenseMap<FileID, SmallDenseMap<apc::Array *, TemplateInfo, 1>>;

  /// Set of declarations.
  using DeclarationSet = DenseSet<Decl *>;

  using DeclarationInfo = DeclarationInfoExtractor::DeclarationInfo;
  using DeclarationInfoMap = DeclarationInfoExtractor::DeclarationInfoMap;

public:
  static char ID;

  APCClangDVMHWriter() : ModulePass(ID) {
    initializeAPCClangDVMHWriterPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override {
    mTfmCtx = nullptr;
    mTransformedFiles.clear();
    mInsertedDirs.clear();
  }

private:
  /// Insert distribution directives for templates into source files.
  ///
  /// This add `#pragma dvm template [...]...[...] distribute [...]...[...]`
  /// directive and declarations (and one definition) for each template:
  /// `[extern] void *Name;`. If template does not used in a file the mentioned
  /// constructs are not inserted in this file.
  /// Definition will be inserted in source file (not include file) only.
  /// \post
  /// - If definition of template has been created then `HasDefinition` flag
  /// is set to true for this template.
  void insertDistibution(const apc::ParallelRegion &Region,
    const apc::DataDirective &DataDirx, TemplateInFileUsage &Templates);

  /// Insert `align` and `array` directives according to a specified align rule
  /// for all redeclarations of a specified variable. Emit diagnostics in case
  /// of errors.
  ///
  // TODO (kaniandr@gmail.com): split declaration statement if it contains
  // multiple declarations.
  // TODO (kaniandr@gmail.com): insert new definition if it is not found,
  // for example we do not treat definitions in include files as definitions
  // and do not insert align directives before such definitions.
  SourceLocation insertAlignment(const ASTImportInfo &Import,
    const DeclarationInfoMap &Decls, const apc::AlignRule &AR,
    const VarDecl *VD);

  /// Insert inherit directive for all redeclarations of a specified function.
  void insertInherit(FunctionDecl *FD, const DeclarationInfoMap &Decls,
    MutableArrayRef<DILocalVariable *> InheritArgs,
    DeclarationSet &NotDistrCanonicalDecls);

  /// Initialize declaration information for global declarations and
  /// collect all canonical declarations (including the local ones).
  ///
  /// \post
  /// - Set `IsSingleDeclStmt` property for global declarations only.
  /// If `Decls` does not contain a declaration then this container will be
  /// updated and declaration will be inserted.
  /// - Canonical declarations for all variable and function declarations will
  /// be stored in `CanonicalDecls` container.
  void initializeDeclInfo(const TranslationUnitDecl &Unit,
    const ASTImportInfo &ImportInfo, DeclarationInfoMap &Decls,
    DeclarationSet &CanonicalDecls);

  /// Insert a specified data directive `DirStr` in a specified location `Where`
  /// or diagnose error if insertion is not possible.
  void insertDataDirective(SourceLocation DeclLoc,
      const DeclarationInfoMap &Decls, SourceLocation Where, StringRef DirStr) {
    assert(Where.isValid() && "Location must be valid!");
    assert(DeclLoc.isValid() && "Location must be valid!");
    auto &Diags = mTfmCtx->getContext().getDiagnostics();
    if (Where.isMacroID()) {
      toDiag(Diags, Where, tsar::diag::err_apc_insert_dvm_directive)
          << DirStr.trim();
      toDiag(Diags, DeclLoc, tsar::diag::note_apc_insert_macro_prevent);
      return;
    }
    auto DInfoItr = Decls.find(DeclLoc.getRawEncoding());
    // DeclarationInfo is not available for functions.
    if (DInfoItr == Decls.end() || DInfoItr->second.IsSingleDeclStmt) {
      insertDirective(Where, DirStr);
    } else {
      toDiag(Diags, Where, tsar::diag::err_apc_insert_dvm_directive)
          << DirStr.trim();
      toDiag(Diags, DeclLoc, tsar::diag::note_apc_not_single_decl_stmt);
    }
  }

  /// Insert a specified directive in a specified location or diagnose error
  /// if other directive has been already inserted at the same point.
  void insertDirective(SourceLocation Where, StringRef DirStr) {
    assert(Where.isValid() && "Location must be valid!");
    assert(Where.isFileID() && "Location must not be in macro!");
    Where = getLocationToTransform(Where);
    assert(Where.isValid() && "Location must be valid!");
    auto Itr = mInsertedDirs.find(Where.getRawEncoding());
    if (Itr != mInsertedDirs.end()) {
      if (Itr->second == DirStr)
        return;
      auto &Diags = mTfmCtx->getContext().getDiagnostics();
      toDiag(Diags, Where, tsar::diag::err_apc_insert_dvm_directive)
          << DirStr.trim();
      toDiag(Diags, Where, tsar::diag::note_apc_insert_multiple_directives);
      return;
    }
    auto &Rwr = mTfmCtx->getRewriter();
    Rwr.InsertTextBefore(Where, DirStr);
    auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
    if (Where != getStartOfLine(Where, SrcMgr))
      Rwr.InsertTextBefore(Where, "\n");
    mTransformedFiles.try_emplace(
      SrcMgr.getFilename(Where), SrcMgr.getFileID(Where));
    mInsertedDirs.try_emplace(Where.getRawEncoding(), DirStr);
  }

  /// If file which contains a specified location `Loc` has been already
  /// transformed return location which points to the same point as `Loc` in
  /// a transformed file.
  SourceLocation getLocationToTransform(SourceLocation Loc) {
    assert(Loc.isValid() && "Location must be valid!");
    assert(Loc.isFileID() && "Location must not be in macro!");
    auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
    auto Filename = SrcMgr.getFilename(Loc);
    assert(!Filename.empty() && "File must be known for a specified location!");
    auto FileItr = mTransformedFiles.find(Filename);
    if (FileItr == mTransformedFiles.end())
      return Loc;
    auto DecLoc = SrcMgr.getDecomposedLoc(Loc);
    auto FileStartLoc = SrcMgr.getLocForStartOfFile(FileItr->second);
    return FileStartLoc.getLocWithOffset(DecLoc.second);
  }

  /// Check that declaration which should not be distributed are not
  /// corrupted by distribution directives.
  template<class DeclT> void checkNotDistributedDecl(const DeclT *D) {
    auto &Ctx = mTfmCtx->getContext();
    auto &SrcMgr = Ctx.getSourceManager();
    auto &Diags = Ctx.getDiagnostics();
    for (auto *Redecl : D->getFirstDecl()->redecls()) {
      auto StartOfDecl = Redecl->getBeginLoc();
      // We have not inserted directives in a macro.
      if (StartOfDecl.isMacroID())
        continue;
      auto Filename = SrcMgr.getFilename(StartOfDecl);
      auto TfmFileItr = mTransformedFiles.find(Filename);
      if (TfmFileItr == mTransformedFiles.end())
        continue;
      auto DecLoc = SrcMgr.getDecomposedLoc(StartOfDecl);
      StartOfDecl = SrcMgr.getLocForStartOfFile(TfmFileItr->second).
        getLocWithOffset(DecLoc.second);
      // Whether a distribution pragma acts on this declaration?
      if (mInsertedDirs.count(StartOfDecl.getRawEncoding()))
        toDiag(Diags, StartOfDecl,
               tsar::diag::err_apc_not_distr_decl_directive);
    }
  }

  /// Check that declarations which should not be distributed are not
  /// corrupted by distribution directives.
  void checkNotDistributedDecls(const DeclarationSet &NotDistrCanonicalDecls) {
    for (auto *D : NotDistrCanonicalDecls)
      if (isa<VarDecl>(D))
        checkNotDistributedDecl(cast<VarDecl>(D));
      else if (isa<FunctionDecl>(D))
        checkNotDistributedDecl(cast<FunctionDecl>(D));
      else
        llvm_unreachable("Unsupported kind of declaration");
  }

  /// List of already transformed files.
  ///
  /// We should not transform different representations of the same files.
  /// For example, if a file has been included twice Rewriter does not allow
  /// to transform it twice.
  StringMap<FileID> mTransformedFiles;

  /// List of already inserted directives at specified locations.
  DenseMap<unsigned, std::string> mInsertedDirs;

  ClangTransformationContext *mTfmCtx = nullptr;
};

using APCClangDVMHWriterProvider = FunctionPassProvider<
  TransformationEnginePass,
  MemoryMatcherImmutableWrapper,
  ClangDIMemoryMatcherPass>;
}

char APCClangDVMHWriter::ID = 0;

namespace {
class APCClangDVMHWriterInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override {
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createAPCContextStorage());
    Passes.add(createDVMHParallelizationContext());
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createGlobalsAccessStorage());
    Passes.add(createGlobalsAccessCollector());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(new DVMHMemoryServer);
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createMemoryMatcherPass());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createAPCArrayInfoPass());
    // End of the second step of analysis on server.
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createAPCLoopInfoBasePass());
    Passes.add(createAPCFunctionInfoPass());
    // End of the third step of analysis on server.
    Passes.add(createAPCDataDistributionPass());
    Passes.add(createDVMHDataTransferIPOPass());
    Passes.add(createClangDVMHWriter());
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};
}

INITIALIZE_PROVIDER(DVMHMemoryServerProvider, "apc-dvmh-server-provider",
  "DVMH Parallelization (APC, Server, Provider)")

template<> char DVMHMemoryServerResponse::ID = 0;
INITIALIZE_PASS(DVMHMemoryServerResponse, "apc-dvmh-server-response",
  "DVMH Parallelization (APC, Server, Response)", true, false)

char DVMHMemoryServer::ID = 0;
INITIALIZE_PASS(DVMHMemoryServer, "dvmh-apc-server",
  "DVMH Parallelization (APC, Server)", false, false)

INITIALIZE_PROVIDER(APCClangDVMHWriterProvider, "clang-apc-dvmh-provider",
  "DVMH Parallelization (APC, Provider)")

char InitializeProviderPass::ID = 0;
INITIALIZE_PASS(InitializeProviderPass, "apc-dvmh-server-init-provider",
  "DVMH Parallelization (APC, Server, Provider, Initialize)", true, true)

INITIALIZE_PASS_IN_GROUP_BEGIN(APCClangDVMHWriter, "clang-experimental-apc-dvmh",
  "DVMH Parallelization (Clang, APC, Experimental)", false, false,
  TransformationQueryManager::getPassRegistry())
  INITIALIZE_PASS_IN_GROUP_INFO(APCClangDVMHWriterInfo);
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
  INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangDIGlobalMemoryMatcherPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
  INITIALIZE_PASS_DEPENDENCY(ImmutableASTImportInfoPass)
  INITIALIZE_PASS_DEPENDENCY(APCClangDVMHWriterProvider)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
  INITIALIZE_PASS_DEPENDENCY(ClangGlobalInfoPass)
  INITIALIZE_PASS_DEPENDENCY(DVMHMemoryServer)
  INITIALIZE_PASS_DEPENDENCY(DVMHMemoryServerResponse)
  INITIALIZE_PASS_DEPENDENCY(DVMHMemoryServerProvider)
  INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
  INITIALIZE_PASS_DEPENDENCY(InitializeProviderPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_IN_GROUP_END(APCClangDVMHWriter, "clang-experimental-apc-dvmh",
  "DVMH Parallelization (Clang, APC, Experimental)", false, false,
  TransformationQueryManager::getPassRegistry())

ModulePass * llvm::createAPCClangDVMHWriter() { return new APCClangDVMHWriter; }

void APCClangDVMHWriter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<DVMHParallelizationContext>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangDIGlobalMemoryMatcherPass>();
  AU.addRequired<APCClangDVMHWriterProvider>();
  AU.addUsedIfAvailable<ImmutableASTImportInfoPass>();
  AU.addRequired<ClangGlobalInfoPass>();
  AU.setPreservesAll();
}

bool APCClangDVMHWriter::runOnModule(llvm::Module &M) {
  releaseMemory();
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  mTfmCtx = TfmInfo ? TfmInfo->getContext(M) : nullptr;
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
      ": transformation context is not available");
    return false;
  }
  APCClangDVMHWriterProvider::initialize<TransformationEnginePass>(
    [&M, &TfmInfo](TransformationEnginePass &TEP) {
    TEP.set(*TfmInfo);
  });
  auto &MatchInfo = getAnalysis<MemoryMatcherImmutableWrapper>().get();
  APCClangDVMHWriterProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MatchInfo](MemoryMatcherImmutableWrapper &Matcher) {
      Matcher.set(MatchInfo);
  });
  ASTImportInfo ImportStub;
  const auto *Import = &ImportStub;
  if (auto *ImportPass = getAnalysisIfAvailable<ImmutableASTImportInfoPass>())
    Import = &ImportPass->getImportInfo();
  auto &GIP = getAnalysis<ClangGlobalInfoPass>();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  auto &APCRegion = APCCtx.getDefaultRegion();
  auto &DataDirs = APCRegion.GetDataDir();
  DenseSet<const AlignRule *> GlobalArrays;
  DenseMap<DISubprogram *, SmallVector<const AlignRule *, 16>> LocalVariables;
  for (auto &AR : DataDirs.alignRules) {
    auto *APCSymbol = AR.alignArray->GetDeclSymbol();
    assert(APCSymbol && "Symbol must not be null!");
    auto *DIVar =
        cast<DIEstimateMemory>(APCSymbol->getVariable().get<MD>().front())
            ->getVariable();
    if (isa<DIGlobalVariable>(DIVar)) {
      GlobalArrays.insert(&AR);
      continue;
    }
    assert(isa<DILocalVariable>(DIVar) && "It must be a local variable!");
    auto Scope = DIVar->getScope();
    while (Scope && !isa<DISubprogram>(Scope))
      Scope = Scope->getScope();
    assert(Scope && "Local variable must be declared in a subprogram!");
    auto LocalItr = LocalVariables.try_emplace(cast<DISubprogram>(Scope)).first;
    LocalItr->second.push_back(&AR);
  }
  DeclarationInfoMap Decls;
  DeclarationSet NotDistrCanonicalDecls;
  auto *Unit = mTfmCtx->getContext().getTranslationUnitDecl();
  initializeDeclInfo(*Unit, *Import, Decls, NotDistrCanonicalDecls);
  TemplateInFileUsage Templates;
  auto insertAlignAndCollectTpl =
    [this, Import, &Templates, &Decls](
      VarDecl &VD, const apc::AlignRule &AR) {
    auto DefLoc = insertAlignment(*Import, Decls, AR, &VD);
    // We should add declaration of template before 'align' directive.
    // So, we remember file with 'align' directive if this directive
    // has been successfully inserted.
    if (DefLoc.isValid()) {
      auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
      auto FID = SrcMgr.getFileID(DefLoc);
      auto TplItr = Templates.try_emplace(FID).first;
      TplItr->second.try_emplace(AR.alignWith);
    }
  };
  for (auto &Info : LocalVariables) {
    auto *F = M.getFunction(Info.first->getName());
    if (!F || F->getSubprogram() != Info.first)
      F = M.getFunction(Info.first->getLinkageName());
    assert(F && F->getSubprogram() == Info.first &&
      "LLVM IR function with attached metadata must not be null!");
    auto &Provider = getAnalysis<APCClangDVMHWriterProvider>(*F);
    auto &Matcher = Provider.get<ClangDIMemoryMatcherPass>().getMatcher();
    auto *FD = cast<FunctionDecl>(mTfmCtx->getDeclForMangledName(F->getName()));
    assert(FD && "AST-level function declaration must not be null!");
    DeclarationInfoExtractor Visitor(Decls);
    Visitor.TraverseFunctionDecl(FD);
    SmallVector<DILocalVariable *, 8> InheritArgs;
    for (auto *AR : Info.second) {
      auto *APCSymbol = AR->alignArray->GetDeclSymbol();
      auto *DIVar = cast<DILocalVariable>(
          cast<DIEstimateMemory>(APCSymbol->getVariable().get<MD>().front())
              ->getVariable());
      auto Itr = Matcher.find<MD>(DIVar);
      assert(Itr != Matcher.end() && "Source-level location must be available!");
      if (DIVar->isParameter())
        InheritArgs.push_back(DIVar);
      else
        insertAlignAndCollectTpl(*Itr->get<AST>(), *AR);
      NotDistrCanonicalDecls.erase(Itr->get<AST>());
    }
    insertInherit(FD, Decls, InheritArgs, NotDistrCanonicalDecls);
  }
  auto &GlobalMatcher =
    getAnalysis<ClangDIGlobalMemoryMatcherPass>().getMatcher();
  for (auto *AR : GlobalArrays) {
    auto *APCSymbol = AR->alignArray->GetDeclSymbol();
    auto *DIVar =
        cast<DIEstimateMemory>(APCSymbol->getVariable().get<MD>().front())
            ->getVariable();
    auto Itr = GlobalMatcher.find<MD>(DIVar);
    assert(Itr != GlobalMatcher.end() &&
      "Source-level location must be available!");
    insertAlignAndCollectTpl(*Itr->get<AST>(), *AR);
    NotDistrCanonicalDecls.erase(Itr->get<AST>());
  }
  checkNotDistributedDecls(NotDistrCanonicalDecls);
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  DenseMap<dvmh::Template *, apc::Array *> TemplatesMap;
  for (auto &&[Tpl, Variant] : DataDirs.distrRules) {
    if (!Tpl->IsTemplate())
      continue;
    TemplatesMap.try_emplace(Tpl->GetDeclSymbol()->getTemplate(), Tpl);
  }
  auto visitRealign = [&TemplatesMap, &Templates](PragmaRealign *Realign,
                                                  FileID FID) {
    if (!Realign)
      return;
    if (auto *Tpl{std::get_if<Template *>(&Realign->with().Target)}) {
      auto *OriginTpl{(**Tpl).getOrigin()};
      OriginTpl = OriginTpl ? OriginTpl : *Tpl;
      auto TplItr{Templates.try_emplace(FID).first};
      TplItr->second.try_emplace(TemplatesMap[OriginTpl]);
    }
  };
  // Search for 'realign' directives. We have to add a template declaration
  // into a file if it contains realign to the template.
  for (auto &&[BB, PLocList] : ParallelCtx.getParallelization()) {
    auto FuncDecl{mTfmCtx->getDeclForMangledName(BB->getParent()->getName())};
    assert(FuncDecl && "Declaration must exist for transformed function!");
    auto &SrcMgr{mTfmCtx->getContext().getSourceManager()};
    auto FID{SrcMgr.getFileID(FuncDecl->getBeginLoc())};
    for (auto &PLoc : PLocList) {
      for (auto &PI : PLoc.Entry)
        visitRealign(dyn_cast<PragmaRealign>(PI.get()), FID);
      for (auto &PI : PLoc.Exit)
        visitRealign(dyn_cast<PragmaRealign>(PI.get()), FID);
      }
  }
  insertDistibution(APCRegion, DataDirs, Templates);
  for (auto &TplInfo : DataDirs.distrRules) {
    GIP.getRawInfo().Identifiers.insert(TplInfo.first->GetShortName());
    for (auto &&[Dist, Name]  : TplInfo.first->GetAllClones())
      GIP.getRawInfo().Identifiers.insert(Name);
  }
  return false;
}

void APCClangDVMHWriter::initializeDeclInfo(const TranslationUnitDecl &Unit,
    const ASTImportInfo &ImportInfo, DeclarationInfoMap &Decls,
    DeclarationSet &CanonicalDecls) {
  DenseMap<unsigned, SourceLocation> FirstGlobalAtLoc;
  auto checkSingleDecl = [&FirstGlobalAtLoc, &Decls](
      SourceLocation StartLoc, SourceLocation Loc) {
    auto Info =
      FirstGlobalAtLoc.try_emplace(StartLoc.getRawEncoding(), Loc);
    if (!Info.second) {
      Decls[Info.first->second.getRawEncoding()].IsSingleDeclStmt = false;
      Decls[Loc.getRawEncoding()].IsSingleDeclStmt = false;
    } else {
      Decls[Loc.getRawEncoding()].IsSingleDeclStmt = true;
    }
  };
  for (auto *D : Unit.decls()) {
    if (auto *FD = dyn_cast<FunctionDecl>(D)) {
      CanonicalDecls.insert(FD->getCanonicalDecl());
      if (FD->hasBody())
        for (auto *D : FD->decls())
          if (isa<VarDecl>(D)) {
            CanonicalDecls.insert(D->getCanonicalDecl());
            Decls[D->getLocation().getRawEncoding()].IsSingleDeclStmt = false;
          }
    } else if (auto *VD = dyn_cast<VarDecl>(D)) {
      CanonicalDecls.insert(VD->getCanonicalDecl());
      auto MergedLocItr = ImportInfo.RedeclLocs.find(VD);
      if (MergedLocItr == ImportInfo.RedeclLocs.end()) {
        checkSingleDecl(VD->getBeginLoc(), VD->getLocation());
      } else {
        auto &StartLocs = MergedLocItr->second.find(VD->getBeginLoc());
        auto &Locs = MergedLocItr->second.find(VD->getLocation());
        for (std::size_t I = 0, EI = Locs.size(); I < EI; ++I)
          checkSingleDecl(StartLocs[I], Locs[I]);
      }
    }
  }
}

void APCClangDVMHWriter::insertInherit(FunctionDecl *FD,
    const DeclarationInfoMap &Decls,
    MutableArrayRef<DILocalVariable *> InheritArgs,
    DeclarationSet &NotDistrCanonicalDecls) {
  if (InheritArgs.empty())
    return;
  sort(InheritArgs,
       [](auto &LHS, auto &RHS) { return LHS->getName() < RHS->getName(); });
  NotDistrCanonicalDecls.erase(FD->getCanonicalDecl());
  for (auto *Redecl : FD->getFirstDecl()->redecls()) {
    SmallString<64> Inherit;
    getPragmaText(DirectiveId::DvmInherit, Inherit);
    Inherit.pop_back();
    Inherit += "(";
    SmallVector<std::pair<SourceLocation, StringRef>, 8> UnnamedArgsInMacro;
    auto insert = [this, Redecl, &Inherit, &UnnamedArgsInMacro](
        const DILocalVariable *DIArg) {
      auto Param = Redecl->getParamDecl(DIArg->getArg() - 1);
      assert(Param && "Parameter must not be null!");
      if (Param->getName().empty()) {
        Inherit += DIArg->getName();
        auto Loc = Param->getLocation();
        if (Loc.isMacroID()) {
          UnnamedArgsInMacro.emplace_back(Loc, DIArg->getName());
        } else {
          SmallVector<char, 16> Name;
          // We add brackets due to the following case
          // void foo(double *A);createOptimalDistribution
          // #define M
          // void foo(double *M);
          // Without brackets we obtain 'void foo(double *MA)' instead of
          // 'void foo(double *M(A))' and do not obtain 'void foo(double *A)'
          // after preprocessing.
          mTfmCtx->getRewriter().InsertTextBefore(Loc,
            ("(" + DIArg->getName() + ")").toStringRef(Name));
        }
      } else {
        Inherit += Param->getName();
      }
    };
    insert(InheritArgs.front());
    for (unsigned I = 1, EI = InheritArgs.size(); I < EI; ++I) {
      Inherit += ",";
      insert(InheritArgs[I]);
    }
    Inherit += ")\n";
    if (!UnnamedArgsInMacro.empty()) {
      auto &Diags = mTfmCtx->getContext().getDiagnostics();
      toDiag(Diags, Redecl->getBeginLoc(),
        tsar::diag::err_apc_insert_dvm_directive) << StringRef(Inherit).trim();
      for (auto &Arg : UnnamedArgsInMacro)
        toDiag(Diags, Arg.first, tsar::diag::note_decl_insert_macro_prevent) <<
          Arg.second;
    }
    insertDataDirective(Redecl->getLocation(), Decls,
      Redecl->getBeginLoc(), Inherit);
  }
}

SourceLocation APCClangDVMHWriter::insertAlignment(const  ASTImportInfo &Import,
    const DeclarationInfoMap &Decls, const apc::AlignRule &AR,
    const VarDecl *VD) {
  // Obtain `#pragma dvm array align` clause.
  SmallString<128> Align;
  getPragmaText(ClauseId::DvmAlign, Align);
  Align.pop_back();
  Align += "(";
  // Add dimension which should be aligned '... [...]...'
  for (std::size_t I = 0, EI = AR.alignRule.size(); I < EI; ++I) {
    assert((AR.alignRule[I].first == 0 || AR.alignRule[I].first == 1) &&
      AR.alignRule[I].second == 0 && "Invalid align rule!");
    Align += "[";
    if (AR.alignRule[I].first == 1 && AR.alignRule[I].second == 0)
     Align += AR.alignNames[I];
    Align += "]";
  }
  auto TplDimAR = extractTplDimsAlignmentIndexes(AR);
  // Add " ... with <template>[...]...[...]".
  Align += " with ";
  Align += AR.alignWith->GetShortName();
  for (auto DimARIdx : TplDimAR) {
    Align += "[";
    if (DimARIdx < TplDimAR.size()) {
      Align += genStringExpr(AR.alignNames[DimARIdx],
                             AR.alignRuleWith[DimARIdx].second);
    }
    Align += "]";
  }
  Align += ")";
  auto Shadows{AR.alignArray->GetShadowSpec()};
  if (auto I{find_if(Shadows,
                     [](auto &S) { return S.first != 1 || S.second != 1; })};
      I != Shadows.end()) {
    Align += " ";
    Align += getName(ClauseId::DvmShadow);
    for (auto &S : Shadows) {
      Align += "[";
      Twine(S.first).toVector(Align);
      if (S.first != S.second) {
        Align += ":";
        Twine(S.second).toVector(Align);
      }
      Align += "]";
    }
  }
  Align += "\n";
  auto &SrcMgr = mTfmCtx->getContext().getSourceManager();
  SourceLocation DefinitionLoc;
  auto *VarDef = VD->getDefinition();
  if (VarDef) {
    DefinitionLoc = VarDef->getLocation();
    auto StartOfDecl = VarDef->getBeginLoc();
    insertDataDirective(DefinitionLoc, Decls, StartOfDecl, Align);
  }
  // Insert 'align' directive before a variable definition (if it is available)
  // and insert 'array' directive before redeclarations of a variable.
  SmallString<16> Array;
  getPragmaText(DirectiveId::DvmArray, Array);
  for (auto *Redecl : VD->getFirstDecl()->redecls()) {
    auto StartOfDecl = Redecl->getBeginLoc();
    auto RedeclLoc = Redecl->getLocation();
    switch (Redecl->isThisDeclarationADefinition()) {
    case VarDecl::Definition: break;
    case VarDecl::DeclarationOnly:
      insertDataDirective(RedeclLoc, Decls, StartOfDecl, Array);
      break;
    case VarDecl::TentativeDefinition:
      if (DefinitionLoc.isInvalid()) {
        auto FID = SrcMgr.getFileID(StartOfDecl);
        bool IsInclude = SrcMgr.getDecomposedIncludedLoc(FID).first.isValid();
        if (IsInclude) {
          insertDataDirective(RedeclLoc, Decls, StartOfDecl, Array);
        } else {
          DefinitionLoc = Redecl->getLocation();
          insertDataDirective(RedeclLoc, Decls, StartOfDecl, Align);
        }
      } else {
        DefinitionLoc = Redecl->getLocation();
        insertDataDirective(RedeclLoc, Decls, StartOfDecl, Align);
      }
      break;
    }
    auto RedeclLocItr = Import.RedeclLocs.find(Redecl);
    if (RedeclLocItr != Import.RedeclLocs.end()) {
      auto &Locs = RedeclLocItr->second.find(RedeclLoc);
      auto &StartLocs = RedeclLocItr->second.find(StartOfDecl);
      for (std::size_t I = 0, EI = Locs.size(); I < EI; ++I) {
        if (Locs[I] == RedeclLoc)
          continue;
        insertDataDirective(Locs[I], Decls, StartLocs[I], Array);
      }
    }
  }
  if (DefinitionLoc.isInvalid()) {
    auto &Diags = mTfmCtx->getContext().getDiagnostics();
    toDiag(Diags, VD->getLocation(),
      tsar::diag::err_apc_insert_dvm_directive) << StringRef(Align).trim();
    toDiag(Diags, tsar::diag::note_apc_no_proper_definition) << VD->getName();
  }
  return DefinitionLoc;
}

void APCClangDVMHWriter::insertDistibution(const apc::ParallelRegion &Region,
    const apc::DataDirective &DataDirs, TemplateInFileUsage &Templates) {
  auto &Rwr = mTfmCtx->getRewriter();
  auto &SrcMgr = Rwr.getSourceMgr();
  auto &LangOpts = Rwr.getLangOpts();
  auto &Diags = mTfmCtx->getContext().getDiagnostics();
  SmallPtrSet<apc::Array *, 8> InsertedTemplates;
  for (auto &File : Templates) {
    auto PreInfo =
      Lexer::ComputePreamble(SrcMgr.getBufferData(File.first), LangOpts);
    // Process templates which are used in a current file.
    for (std::size_t DistrRuleIdx = 0,
        DistrRuleEIdx = DataDirs.distrRules.size();
        DistrRuleIdx < DistrRuleEIdx; ++DistrRuleIdx) {
      auto &AllTplDistrRules = DataDirs.distrRules[DistrRuleIdx];
      auto TplUsageItr = File.second.find(AllTplDistrRules.first);
      if (TplUsageItr == File.second.end())
        continue;
      auto *Tpl = TplUsageItr->first;
      auto &TplInfo = TplUsageItr->second;
      auto buildDistribute = [Tpl,
                              &TplInfo](StringRef Name, bool IsDefinition,
                                        auto &DistRule,
                                        SmallVectorImpl<char> &Distribute) {
        // Obtain "#pragma dvm template"
        getPragmaText(DirectiveId::DvmTemplate, Distribute);
        Distribute.pop_back();
        Distribute.push_back(' ');
        // Add size of each template dimension to pragma: "... [Size] ..."
        auto &DimSizes = Tpl->GetSizes();
        for (std::size_t DimIdx = 0, DimIdxE = Tpl->GetDimSize();
             DimIdx < DimIdxE; ++DimIdx) {
          assert(DimSizes[DimIdx].first == 0 &&
                 "Lower dimension bound must be 0 for C language!");
          Distribute.push_back('[');
          SmallString<8> SizeData;
          auto Size{Twine(DimSizes[DimIdx].second).toStringRef(SizeData)};
          Distribute.append(Size.begin(), Size.end());
          Distribute.push_back(']');
        }
        // Add distribution rules according to current distribution variant.
        Distribute.append(
            {' ', 'd', 'i', 's', 't', 'r', 'i', 'b', 'u', 't', 'e', ' '});
        for (auto Kind : DistRule) {
          Distribute.push_back('[');
          switch (Kind) {
          case BLOCK:
            Distribute.append({'b', 'l', 'o', 'c', 'k'});
            break;
          case NONE:
            break;
          default:
            llvm_unreachable("Unknown distribution rule!");
            break;
          }
          Distribute.push_back(']');
        }
        Distribute.push_back('\n');
        // Use `extern` in to avoid variable redefinition.
        if (!IsDefinition)
          Distribute.append({'e', 'x', 't', 'e', 'r', 'n'});
        Distribute.append({'v', 'o', 'i', 'd', ' ', '*'});
        Distribute.append(Name.begin(), Name.end());
        Distribute.push_back(';');
        Distribute.push_back('\n');
      };
      SmallString<256> Distribute;
      auto &DistrVariant = Region.GetCurrentVariant();
      assert(DistrVariant[DistrRuleIdx] < AllTplDistrRules.second.size() &&
        "Variant index must be less than number of variants!");
      auto &DR = AllTplDistrRules.second[DistrVariant[DistrRuleIdx]];
      buildDistribute(Tpl->GetShortName(), !TplInfo.HasDefinition, DR.distRule,
                      Distribute);
      for (auto &[DistrRule, Name] : Tpl->GetAllClones()) {
        buildDistribute(Name, !TplInfo.HasDefinition, DistrRule, Distribute);
      }
      Distribute.push_back('\n');
      TplInfo.HasDefinition = true;
      auto Where =
        SrcMgr.getLocForStartOfFile(File.first).getLocWithOffset(PreInfo.Size);
      // TODO (kaniandr@gmail.com): do not insert directive in include file
      // if some inclusion locations may be in a local scope. Such check is
      // not implemented, hence we conservatively disable insertion of directive
      // in an include file.
      if (SrcMgr.getDecomposedIncludedLoc(File.first).first.isValid()) {
        toDiag(Diags, Where, tsar::diag::err_apc_insert_dvm_directive) <<
          StringRef(Distribute).trim();
        toDiag(Diags, Where, tsar::diag::note_apc_insert_include_prevent);
      }
      // Insert at the end of preamble.
      Where = getLocationToTransform(Where);
      assert(Where.isFileID() && "Location must not be in macro!");
      Rwr.InsertTextBefore(Where, Distribute);
      mTransformedFiles.try_emplace(SrcMgr.getFilename(Where), File.first);
      InsertedTemplates.insert(Tpl);
    }
  }
  if (DataDirs.distrRules.size() != InsertedTemplates.size())
    for (auto &TplInfo : DataDirs.distrRules)
      if (!InsertedTemplates.count(TplInfo.first))
        toDiag(Diags, tsar::diag::err_apc_insert_template) <<
          TplInfo.first->GetShortName();
}
