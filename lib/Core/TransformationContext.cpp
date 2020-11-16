//===- TransformationContext.cpp - TSAR Transformation Engine ---*- C++ -*-===//
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
// This file implements source level transformation engine which is necessary to
// transform high level and low-level representation of program correlated.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/TransformationContext.h"
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "tsar/Frontend/Clang/TransformationContext.h"
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>

using namespace tsar;
using namespace llvm;

#undef DEBUG_TYPE
#define DEBUG_TYPE "transform"

template<> char TransformationEnginePass::ID = 0;
INITIALIZE_PASS(TransformationEnginePass, "transform",
  "Transformation Engine Accessor", true, true)

ImmutablePass * llvm::createTransformationEnginePass() {
  return new TransformationEnginePass();
}

FilenameAdjuster tsar::getDumpFilenameAdjuster() {
  static FilenameAdjuster FA = [](StringRef Filename) -> std::string {
    static StringMap<unsigned short> DumpFiles;
    auto Pair = DumpFiles.insert(std::make_pair(Filename, 1));
    if (!Pair.second)
      ++Pair.first->getValue();
    auto constexpr MaxDigits =
      std::numeric_limits<unsigned short>::digits10 + 1;
    char Buf[MaxDigits];
    snprintf(Buf, MaxDigits, "%d", Pair.first->getValue());
    SmallString<128> Path = Filename;
    sys::path::replace_extension(Path, Buf + sys::path::extension(Path));
    return std::string(Path);
  };
  return FA;
}

ClangTransformationContext * TransformationInfo::getContext(llvm::Module &M) {
  auto CUs = M.getNamedMetadata("llvm.dbg.cu");
  if (CUs->getNumOperands() > 1)
    return nullptr;
  auto *CU = cast<DICompileUnit>(*CUs->op_begin());
  auto I = mTransformPool.find(CU);
  return I != mTransformPool.end()
             ? dyn_cast<ClangTransformationContext>(I->second.get())
             : nullptr;
}
