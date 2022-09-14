//===--- IRAction.cpp ------- TSAR IR Action --------------------*- C++ -*-===//
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
// This file implements an action to process multi-file project. It loads IR
// from a file, parses corresponding sources and attach them to the
// transformation context.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/IRAction.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/tsar-config.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Frontend/Clang/Action.h"
#include "tsar/Frontend/Clang/FrontendActions.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/SMStringSocket.h"
#include <bcl/IntrusiveConnection.h>
#include <bcl/Json.h>
#include <llvm/Support/Timer.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#ifdef FLANG_FOUND
# include "tsar/Frontend/Flang/Action.h"
# include "tsar/Frontend/Flang/Tooling.h"
# include "tsar/Frontend/Flang/TransformationContext.h"
#endif
#include <vector>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace tsar::detail {
JSON_OBJECT_BEGIN(SourceResponse)
JSON_OBJECT_ROOT_PAIR(SourceResponse, Context,
                      tsar::TransformationContextBase *)
SourceResponse() : JSON_INIT_ROOT {}
JSON_OBJECT_END(SourceResponse)
} // namespace tsar::detail

using namespace tsar::detail;

namespace {
template <TransformationContextBase::Kind FrontendKind> struct ActionHelper {
  IntrusiveRefCntPtr<TransformationContextBase> CreateTransformationContext(
      [[maybe_unused]] const llvm::Module &M,
      [[maybe_unused]] const DICompileUnit &CU,
      [[maybe_unused]] StringRef IRSource, [[maybe_unused]] StringRef Path,
      [[maybe_unused]] const tooling::CompilationDatabase &Compilations) {
    return nullptr;
  }
};

class ASTSocket final : public SMStringSocketBase<ASTSocket> {
public:
  void processResponse(const std::string &Response) const {
    llvm::StringRef Json{Response.data() + 1, Response.size() - 2};
    ::json::Parser<SourceResponse> Parser(Json.str());
    SourceResponse R;
    if (!Parser.parse(R))
      mTfmCtx = nullptr;
    else
      mTfmCtx = R[SourceResponse::Context];
  }

  auto getContext() {
    for (auto &Callback : mReceiveCallbacks)
      Callback({Data, Delimiter});
    // Note, that callback run send() in client, so result is already set here.
    assert(mResponseKind == Data && "Unknown response: wait for data!");
    return IntrusiveRefCntPtr<TransformationContextBase>(mTfmCtx);
  }

private:
  mutable TransformationContextBase *mTfmCtx{nullptr};
};

class SourceQueryManager : public QueryManager {
public:
  explicit SourceQueryManager(bcl::IntrusiveConnection *C) : mConnection(C) {
    assert(C && "Connection must not be null!");
  }
  void run(llvm::Module *M, TransformationInfo *TfmInfo) override {
    bool WaitForRequest{true};
    while (WaitForRequest &&
           mConnection->answer([&WaitForRequest, M,
                                TfmInfo](std::string &Request) -> std::string {
             if (Request == ASTSocket::Release) {
               WaitForRequest = false;
               return {ASTSocket::Notify};
             } else if (Request == ASTSocket::Data) {
               auto CUs{M->getNamedMetadata("llvm.dbg.cu")};
               assert(CUs && "DICompileUnit metadata must exist!");
               auto I{find_if(CUs->operands(),
                              [](auto *Op) { return isa<DICompileUnit>(Op); })};
               assert(I != CUs->operands().end() &&
                      "DICompileUnit metadata must exist!");
               SourceResponse Response;
               Response[SourceResponse::Context] =
                   TfmInfo->getContext(*cast<DICompileUnit>(*I));
               return ASTSocket::Data +
                      ::json::Parser<SourceResponse>::unparseAsObject(Response);
             } else {
               return {ASTSocket::Invalid};
             }
           }))
      ;
  }

private:
  bcl::IntrusiveConnection *mConnection;
};

template<>
struct ActionHelper<TransformationContextBase::TC_Clang> {
  ~ActionHelper() {
    for (auto &S : mSockets)
      S->release();
  }

  IntrusiveRefCntPtr<TransformationContextBase> CreateTransformationContext(
      [[maybe_unused]] const llvm::Module &M,
      [[maybe_unused]] const DICompileUnit &CU,
      [[maybe_unused]] StringRef IRSource, StringRef Path,
      const tooling::CompilationDatabase &Compilations) {
    mSockets.push_back(std::make_unique<ASTSocket>());
    bcl::IntrusiveConnection::connect(
        mSockets.back().get(), ASTSocket::Delimiter,
        [this, &Compilations, &Path](bcl::IntrusiveConnection C) {
          tooling::ClangTool CTool(Compilations, makeArrayRef(Path.str()));
          SourceQueryManager SQM{&C};
          CTool.run(newClangActionFactory<ClangMainAction, GenPCHPragmaAction>(
                        std::forward_as_tuple(Compilations,
                                              static_cast<QueryManager &>(SQM)))
                        .get());
        });
    return mSockets.back()->getContext();
  }

private:
  std::vector<std::unique_ptr<ASTSocket>> mSockets;
};

#ifdef FLANG_FOUND
template<>
struct ActionHelper<TransformationContextBase::TC_Flang> {
  ~ActionHelper() {
    for (auto &S : mSockets)
      S->release();
  }

  IntrusiveRefCntPtr<TransformationContextBase> CreateTransformationContext(
      [[maybe_unused]] const llvm::Module &M,
      [[maybe_unused]] const DICompileUnit &CU,
      [[maybe_unused]] StringRef IRSource, StringRef Path,
      const tooling::CompilationDatabase &Compilations) {
    mSockets.push_back(std::make_unique<ASTSocket>());
    bcl::IntrusiveConnection::connect(
        mSockets.back().get(), ASTSocket::Delimiter,
        [this, &Compilations, &Path](bcl::IntrusiveConnection C) {
          FlangTool FortranTool(Compilations, makeArrayRef(Path.str()));
          SourceQueryManager SQM{&C};
          FortranTool.run(
              newFlangActionFactory<FlangMainAction>(
                  std::forward_as_tuple(Compilations,
                                        static_cast<QueryManager &>(SQM)))
                  .get());
        });
    return mSockets.back()->getContext();
  }

private:
  std::vector<std::unique_ptr<ASTSocket>> mSockets;
};
#endif
}

namespace json {
template <>
struct CellTraits<tsar::detail::json_::SourceResponseImpl::Context> {
  using CellKey = tsar::detail::json_::SourceResponseImpl::Context;
  using ValueType = CellKey::ValueType;
  inline static bool parse(ValueType &Dest, Lexer &Lex)
      noexcept(
        noexcept(Traits<ValueType>::parse(Dest, Lex))) {
    uintptr_t RawDest;
    auto Res = Traits<uintptr_t>::parse(RawDest, Lex);
    if (Res)
      Dest = reinterpret_cast<tsar::TransformationContextBase *>(RawDest);
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
}

JSON_DEFAULT_TRAITS(::, SourceResponse)

int tsar::executeIRAction(StringRef ToolName, ArrayRef<std::string> Sources,
                          QueryManager &QM,
                          const tooling::CompilationDatabase *Compilations) {
  std::size_t IsOk{Sources.size()};
  Timer ASTGeneration("ASTGeneration", "AST Generation Time");
  Timer LLVMIRAnalysis("LLVMIRAnalysis", "LLVM IR Analysis Time");
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID{new DiagnosticIDs};
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts{new DiagnosticOptions};
  auto *DiagsPrinter{new clang::TextDiagnosticPrinter{errs(), &*DiagOpts}};
  DiagnosticsEngine Diags{DiagID, &*DiagOpts, DiagsPrinter};
  for (auto &File : Sources) {
    SMDiagnostic Err;
    LLVMContext Ctx;
    auto M{parseIRFile(File, Err, Ctx)};
    if (!M) {
      --IsOk;
      Err.print(ToolName.data(), errs());
      continue;
    }
    SmallString<128> Path{File};
    sys::fs::make_absolute(Path);
    sys::path::native(Path);
    llvm::sys::path::remove_filename(Path);
    if (Compilations) {
      if (TimePassesIsEnabled)
        ASTGeneration.startTimer();
      TransformationInfo TfmInfo(*Compilations);
      ActionHelper<TransformationContextBase::TC_Clang> ClangHelper;
      ActionHelper<TransformationContextBase::TC_Flang> FlangHelper;
      auto CUs = M->getNamedMetadata("llvm.dbg.cu");
      for (auto *Op : CUs->operands())
        if (auto *CU = dyn_cast<DICompileUnit>(Op)) {
          SmallString<128> Path{CU->getFilename()};
          sys::fs::make_absolute(CU->getDirectory(), Path);
          if (isFortran(CU->getSourceLanguage())) {
            if (auto TfmCtx{FlangHelper.CreateTransformationContext(
                    *M, *CU, File, Path, *Compilations)})
              TfmInfo.setContext(*CU, std::move(TfmCtx));
          } else if (isC(CU->getSourceLanguage()) ||
                     isCXX(CU->getSourceLanguage()))
            if (auto TfmCtx{ClangHelper.CreateTransformationContext(
                    *M, *CU, File, Path, *Compilations)})
              TfmInfo.setContext(*CU, std::move(TfmCtx));
        }
      if (TimePassesIsEnabled) {
        ASTGeneration.stopTimer();
        LLVMIRAnalysis.startTimer();
      }
      QM.beginSourceFile(Diags, File, "", Path);
      QM.run(M.get(), &TfmInfo);
      QM.endSourceFile(false);
      if (TimePassesIsEnabled)
        LLVMIRAnalysis.stopTimer();
    } else {
      if (TimePassesIsEnabled)
        LLVMIRAnalysis.startTimer();
      QM.beginSourceFile(Diags, File, "", Path);
      QM.run(M.get(), nullptr);
      QM.endSourceFile(false);
      if (TimePassesIsEnabled)
        LLVMIRAnalysis.stopTimer();
    }
  }
  return IsOk != Sources.size() ? IsOk != 0 ? 2 : 1 : 0;
}
