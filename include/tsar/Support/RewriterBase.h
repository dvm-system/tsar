//===- RewriterBase.h ----------TSAR Rewriter Base --------------*- C++ -*-===//
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

#ifndef TSAR_REWRITER_BASE_H
#define TSAR_REWRITER_BASE_H

#include <llvm/ADT/StringRef.h>
#include <vector>

namespace tsar {
/// This class provides methods to implements rewriters in the same way as
/// clang::Rewriter is implemented.
class RewriterBaseImpl {
public:
  /// Return current state of the text in the initial range.
  llvm::StringRef getBuffer() const { return mBuffer; }

  /// Replace a range of characters in the buffer with a new string, return
  /// `false` on success and `true` in case of errors.
  void ReplaceText(std::size_t OrigBegin, std::size_t Length,
                   llvm::StringRef NewStr);

  /// Insert a specified string at a specified location in the buffer,
  /// return `false` on success and `true` in case of errors.
  void InsertText(std::size_t OrigBegin, llvm::StringRef NewStr,
                  bool InsertAfter);

  /// Return the rewritten form of the text in the specified range
  /// inside the initial range.
  llvm::StringRef getRewrittenText(std::size_t OrigBegin, std::size_t Length) {
    auto Begin{mMapping[2 * OrigBegin]};
    auto End{mMapping[2 * (OrigBegin + Length - 1) + 1] + 1};
    return llvm::StringRef(mBuffer.data() + Begin, End - Begin);
  }

protected:
  std::string mBuffer;
  std::vector<std::size_t> mMapping;
};

template<class RewriterT, class RewriterInfoT>
class RewriterBase : public RewriterBaseImpl {
public:
  using SourceLocationT = typename RewriterInfoT::SourceLocationT;
  using SourceRangeT = typename RewriterInfoT::SourceRangeT;

  /// Replace a range of characters in the buffer with a new string, return
  /// `false` on success and `true` in case of errors.
  bool ReplaceText(SourceRangeT SR, llvm::StringRef NewStr) {
    return static_cast<RewriterT *>(this)->ReplaceText(SR, NewStr);
  }

  /// Insert a specified string at a specified location in the buffer,
  /// return `false` on success and `true` in case of errors.
  bool InsertText(SourceLocationT Loc, llvm::StringRef NewStr,
                  bool InsertAfter = true) {
    return static_cast<RewriterT *>(this)->InsertText(Loc, NewStr);
  }

  /// Insert a specified string at a specified location in the buffer,
  /// return `false` on success and `true` in case of errors.
  ///
  /// The text is inserted before the specified location. This is
  /// method is the same as InsertText with "InsertAfter == false".
  bool InsertTextBefore(SourceLocationT Loc, llvm::StringRef NewStr) {
    return InsertText(Loc, NewStr, false);
  }

  /// Insert a specified string at a specified location in the buffer,
  /// return `false` on success and `true` in case of errors.
  ///
  /// Text is inserted after any other text that has been previously inserted
  /// at the some point (the default behavior for InsertText).
  bool InsertTextAfter(SourceLocationT Loc, llvm::StringRef NewStr) {
    return InsertText(Loc, NewStr, true);
  }
};
}
#endif//TSAR_REWRITER_BASE_H
