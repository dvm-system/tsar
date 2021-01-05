//===- RewriterBase.cpp---------TSAR Rewriter Base --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file provides classes to implements rewriters in the same way as
// clang::Rewriter is implemented.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/RewriterBase.h"

void tsar::RewriterBaseImpl::ReplaceText(
    std::size_t OrigBegin, std::size_t Length, llvm::StringRef NewStr) {
  std::size_t OrigBeginIdx = 2 * OrigBegin;
  std::size_t OrigEndIdx = 2 * (OrigBegin + Length - 1) + 1;
  auto Begin = mMapping[OrigBeginIdx];
  auto End = mMapping[OrigEndIdx] + 1;
  auto NewStrSize = NewStr.size();
  if (End - Begin < NewStrSize) {
    for (std::size_t I = OrigEndIdx, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] += NewStrSize - (End - Begin);
  } else if (End - Begin > NewStrSize) {
    for (std::size_t I = OrigEndIdx, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] -= (End - Begin) - NewStrSize;
  }
  mBuffer.replace(Begin, End - Begin, std::string(NewStr));
}

void tsar::RewriterBaseImpl::InsertText(
    std::size_t OrigBegin, llvm::StringRef NewStr, bool InsertAfter) {
  std::size_t OrigIdx = InsertAfter ? 2 * OrigBegin + 1 : 2 * OrigBegin;
  mBuffer.insert(mMapping[OrigIdx], NewStr.str());
  auto NewStrSize = NewStr.size();
  for (std::size_t I = OrigIdx, EI = mMapping.size(); I < EI; ++I)
    mMapping[I] += NewStrSize;
}

