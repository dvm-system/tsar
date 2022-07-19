//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
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
// This file implements abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/Utils.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/OutputFile.h"
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Errc.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <regex>

using namespace llvm;
using namespace tsar;

namespace tsar {
std::vector<StringRef> tokenize(StringRef Str, StringRef Pattern) {
  std::vector<StringRef> Tokens;
  std::regex Rgx(Pattern.data());
  std::cmatch Cm;
  if (!std::regex_search(Str.data(), Cm, Rgx))
    return Tokens;
  bool HasSubMatch = false;
  for (std::size_t I = 1; I < Cm.size(); ++I)
    if (Cm[I].matched) {
      HasSubMatch = true;
      Tokens.emplace_back(Cm[I].first, Cm[I].length());
    }
  if (!HasSubMatch)
    Tokens.emplace_back(Cm[0].first, Cm[0].length());
  while (std::regex_search(Cm[0].second, Cm, Rgx)) {
    bool HasSubMatch = false;
    for (std::size_t I = 1; I < Cm.size(); ++I)
      if (Cm[I].matched) {
        HasSubMatch = true;
        Tokens.emplace_back(Cm[I].first, Cm[I].length());
      }
    if (!HasSubMatch)
      Tokens.emplace_back(Cm[0].first, Cm[0].length());
  }
  return Tokens;
}

llvm::Optional<unsigned> getLanguage(const llvm::DIVariable &DIVar) {
  auto Scope = DIVar.getScope();
  while (Scope) {
    auto *CU = isa<DISubprogram>(Scope) ?
      cast<DISubprogram>(Scope)->getUnit() : dyn_cast<DICompileUnit>(Scope);
    if (CU)
      return CU->getSourceLanguage();
    Scope = Scope->getScope();
  }
  return None;
}

llvm::Optional<uint64_t> getConstantCount(
    const llvm::DISubrange &Range) {
  auto DICount = Range.getCount();
  if (!DICount)
    return None;
  if (!DICount.is<ConstantInt*>())
    return None;
  auto Count = DICount.get<ConstantInt *>()->getValue();
  if (!Count.isStrictlyPositive())
    return None;;
  return Count.getSExtValue();
}

llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo) {
  auto ArgItr = F.arg_begin();
  auto ArgItrE = F.arg_end();
  for (std::size_t I = 0; ArgItr != ArgItrE && I <= ArgNo; ++I, ++ArgItr);
  return ArgItr != ArgItrE ? &*ArgItr : nullptr;
}

bool pointsToLocalMemory(const Value &V, const Loop &L) {
  if (!isa<AllocaInst>(V))
    return false;
  bool StartInLoop{false}, EndInLoop{false};
  auto approveLocal = [&L, &StartInLoop, &EndInLoop](auto *V) {
    if (auto *II{dyn_cast<IntrinsicInst>(V)}) {
      auto *BB{II->getParent()};
      if (L.contains(BB)) {
        auto ID{II->getIntrinsicID()};
        if (!StartInLoop && ID == llvm::Intrinsic::lifetime_start)
          StartInLoop = true;
        else if (!EndInLoop && ID == llvm::Intrinsic::lifetime_end)
          EndInLoop = true;
        if (StartInLoop && EndInLoop)
          return true;
      }
    }
    return false;
  };
  for (auto *V1 : V.users())
    if (approveLocal(V1)) {
      return true;
    } else if (auto *BC{dyn_cast<BitCastInst>(V1)}) {
      for (auto *V2 : BC->users())
        if (approveLocal(V2))
          return true;
    }
  return false;
}

llvm::Type *getPointerElementType(const llvm::Value &V) {
  if (!llvm::isa<llvm::PointerType>(V.getType()))
    return nullptr;
  if (auto *AI{llvm::dyn_cast<llvm::AllocaInst>(&V)})
    return AI->getAllocatedType();
  if (auto *GV{llvm::dyn_cast<llvm::GlobalValue>(&V)})
    return GV->getValueType();
  if (auto *GEP{llvm::dyn_cast<llvm::GEPOperator>(&V)})
     return GEP->getResultElementType();
  for (auto &U : V.uses()) {
    if (auto *LI{llvm::dyn_cast<llvm::LoadInst>(U.getUser())})
      return LI->getType();
    if (auto *SI{llvm::dyn_cast<llvm::StoreInst>(U.getUser())}) {
      if (SI->getPointerOperand() == U)
        return SI->getValueOperand()->getType();
      for (auto &U1 : SI->getPointerOperand()->uses())
        if (auto *LI{llvm::dyn_cast<llvm::LoadInst>(U1.getUser())})
          if (auto *PointeeTy{getPointerElementType(*LI)})
            return PointeeTy;
    }
    if (auto *GEP{llvm::dyn_cast<llvm::GEPOperator>(U.getUser())})
      if (GEP->getPointerOperand() == U)
        return GEP->getSourceElementType();
    if (auto *Cast{llvm::dyn_cast<llvm::BitCastOperator>(U.getUser())})
      if (auto *T{getPointerElementType(*Cast)})
        return T;
    if (auto II{llvm::dyn_cast<llvm::IntrinsicInst>(U.getUser())})
      if (II->isArgOperand(&U))
        if (auto *T{II->getParamElementType(II->getArgOperandNo(&U))})
          return T;
  }
  return nullptr;
}

llvm::DIType *createStubType(llvm::Module &M, unsigned int AS,
                             llvm::DIBuilder &DIB) {
  /// TODO (kaniandr@gmail.com): we create a stub instead of an appropriate
  /// type because type must not be set to nullptr. We mark such type as
  /// artificial type with name "sapfor.type", however may be this is not
  /// a good way to distinguish such types?
  auto DIBasicTy = DIB.createBasicType(
      "char", llvm::Type::getInt1Ty(M.getContext())->getScalarSizeInBits(),
      dwarf::DW_ATE_unsigned_char);
  auto PtrSize = M.getDataLayout().getPointerSizeInBits(AS);
  return DIB.createArtificialType(
      DIB.createPointerType(DIBasicTy, PtrSize, 0, None, "sapfor.type"));
}

Expected<OutputFile>
OutputFile::create(StringRef OutputPath, bool Binary,
                 bool RemoveFileOnSignal, bool UseTemporary,
                 bool CreateMissingDirectories) {
  assert((!CreateMissingDirectories || UseTemporary) &&
         "CreateMissingDirectories is only allowed when using temporary files");

  std::unique_ptr<llvm::raw_fd_ostream> OS;
  Optional<StringRef> OSFile;

  if (UseTemporary) {
    if (OutputPath == "-")
      UseTemporary = false;
    else {
      llvm::sys::fs::file_status Status;
      llvm::sys::fs::status(OutputPath, Status);
      if (llvm::sys::fs::exists(Status)) {
        // Fail early if we can't write to the final destination.
        if (!llvm::sys::fs::can_write(OutputPath))
          return llvm::errorCodeToError(
              make_error_code(llvm::errc::operation_not_permitted));

        // Don't use a temporary if the output is a special file. This handles
        // things like '-o /dev/null'
        if (!llvm::sys::fs::is_regular_file(Status))
          UseTemporary = false;
      }
    }
  }
  Optional<llvm::sys::fs::TempFile> Temp;
  if (UseTemporary) {
    // Create a temporary file.
    // Insert -%%%%%%%% before the extension (if any), and because some tools
    // (noticeable, clang's own GlobalModuleIndex.cpp) glob for build
    // artifacts, also append .tmp.
    StringRef OutputExtension = llvm::sys::path::extension(OutputPath);
    SmallString<128> TempPath =
        StringRef(OutputPath).drop_back(OutputExtension.size());
    TempPath += "-%%%%%%%%";
    TempPath += OutputExtension;
    TempPath += ".tmp";
    Expected<llvm::sys::fs::TempFile> ExpectedFile =
        llvm::sys::fs::TempFile::create(
            TempPath, llvm::sys::fs::all_read | llvm::sys::fs::all_write,
            Binary ? llvm::sys::fs::OF_None : llvm::sys::fs::OF_Text);

    llvm::Error E = handleErrors(
        ExpectedFile.takeError(), [&](const llvm::ECError &E) -> llvm::Error {
          std::error_code EC = E.convertToErrorCode();
          if (CreateMissingDirectories &&
              EC == llvm::errc::no_such_file_or_directory) {
            StringRef Parent = llvm::sys::path::parent_path(OutputPath);
            EC = llvm::sys::fs::create_directories(Parent);
            if (!EC) {
              ExpectedFile = llvm::sys::fs::TempFile::create(TempPath);
              if (!ExpectedFile)
                return llvm::errorCodeToError(
                    llvm::errc::no_such_file_or_directory);
            }
          }
          return llvm::errorCodeToError(EC);
        });

    if (E) {
      consumeError(std::move(E));
    } else {
      Temp = std::move(ExpectedFile.get());
      OS.reset(new llvm::raw_fd_ostream(Temp->FD, /*shouldClose=*/false));
      OSFile = Temp->TmpName;
    }
    // If we failed to create the temporary, fallback to writing to the file
    // directly. This handles the corner case where we cannot write to the
    // directory, but can write to the file.
  }

  if (!OS) {
    OSFile = OutputPath;
    std::error_code EC;
    OS.reset(new llvm::raw_fd_ostream(
        *OSFile, EC,
        (Binary ? llvm::sys::fs::OF_None : llvm::sys::fs::OF_TextWithCRLF)));
    if (EC)
      return llvm::errorCodeToError(EC);
  }

  // Don't try to remove "-", since this means we are using stdin.
  if (!Binary || OS->supportsSeeking())
    return OutputFile{(OutputPath != "-") ? OutputPath : "",
                      Binary,
                      RemoveFileOnSignal,
                      CreateMissingDirectories,
                      std::move(OS),
                      std::move(Temp)};

  return OutputFile{
      (OutputPath != "-") ? OutputPath : "",
      Binary,
      RemoveFileOnSignal,
      CreateMissingDirectories,
      std::make_unique<llvm::buffer_unique_ostream>(std::move(OS)),
      std::move(Temp)};
}

llvm::Error OutputFile::clear(StringRef WorkingDir, bool EraseFile) {
  assert(isValid() && "The file has been already cleared!");
  mOS.reset();
  // Ignore errors that occur when trying to discard the temp file.
  if (EraseFile) {
    if (useTemporary())
      consumeError(mTemp.front().discard());
    if (!mFilename.empty())
      llvm::sys::fs::remove(mFilename);
    mTemp.clear();
    return Error::success();
  }
  if (!useTemporary())
    return Error::success();
  if (getTemporary().TmpName.empty()) {
    consumeError(mTemp.front().discard());
    mTemp.clear();
    return Error::success();
  }
  SmallString<128> NewOutFile{mFilename};
  if (!WorkingDir.empty() && !llvm::sys::path::is_absolute(mFilename)) {
    NewOutFile = WorkingDir;
    llvm::sys::path::append(NewOutFile, mFilename);
  }
  llvm::Error E = mTemp.front().keep(NewOutFile);
  if (!E) {
    mTemp.clear();
    return Error::success();
  }
  llvm::sys::fs::remove(getTemporary().TmpName);
  mTemp.clear();
  return std::move(E);
}
}
