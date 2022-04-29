//===------ Messages.h ------ Traits Static Analyzer ------------*- C++ -*-===//
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
// This defines general messages for server-client interaction, where server
// is TSAR. This also defines some utility entities to simplify definition of
// a new message.
//
// Each message is a JSON string which is parsed and unparsed with JSON String
// Serializer from bcl/Json.h file.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MESSAGES_H
#define TSAR_MESSAGES_H

#include <bcl/cell.h>
#include <bcl/Diagnostic.h>
#include <bcl/Json.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/ADT/Optional.h>
#include <llvm/Support/FileSystem.h>
#include <array>
#include <limits>
#include <vector>
#include <string>

namespace tsar {
namespace msg {
/// Represents general static information about header files.
struct HeaderFile {
  JSON_NAME(header)
  static const std::array<const char *, 2> & extensions() {
    static std::array<const char *, 2> Exts{ { ".h", ".hpp" } };
    return Exts;
  }
};

/// Represents general static information about source files.
struct SourceFile {
  JSON_NAME(source)
  static const std::array<const char *, 3> & extensions() {
    static std::array<const char *, 3> Exts{ {".c", ".cpp", ".cxx"} };
    return Exts;
  }
};

/// Represents general static information about unknown files.
struct OtherFile {
  JSON_NAME(other)
};

/// \brief This defines Status type of field for some messages.
///
/// For example it can be used to provide status of some operation execution.
/// Appropriate JSON serialization rules are defined bellow.
enum class Status : short {
  First = 0,
  Success = First,
  Done,
  Error,
  Last = Error,
  Invalid,
  Number = Invalid
};

/// \brief This defines Analysis type of field for some messages.
///
/// For example it can be used to specify which entities have been analyzed and
/// have not been analyzed.
/// Appropriate JSON serialization rules are defined bellow.
enum class Analysis : short {
  First = 0,
  Yes = First,
  No,
  Last = No,
  Invalid,
  Number = Invalid
};

/// \brief This message provide diagnostic for analysis execution.
///
/// Terminal filed is used to provide data from stdout or/and stderr.
JSON_OBJECT_BEGIN(Diagnostic)
JSON_OBJECT_ROOT_PAIR_4(Diagnostic,
  Status, msg::Status,
  Error, std::vector<std::string>,
  Warning, std::vector<std::string>,
  Terminal, std::string)

  /// Creates new diagnostic of a specified status.
  explicit Diagnostic(msg::Status S) :
    JSON_INIT_ROOT, JSON_INIT(Diagnostic, S) {}

  Diagnostic(const Diagnostic &) = default;
  Diagnostic & operator=(const Diagnostic &) = default;
  Diagnostic(Diagnostic &&) = default;
  Diagnostic & operator=(Diagnostic &&) = default;

  /// \brief Inserts diagnostics of a specified type 'Ty'.
  ///
  /// Usage: Diag.insert(msg::Diagnostic::Error, D).
  /// \pre Type of diagnostic is Error or Wrninng
  template<class Ty> void insert(Ty, const bcl::Diagnostic &D) {
    for (auto Msg : D)
      value<Ty>().push_back(Msg);
  }
JSON_OBJECT_END(Diagnostic)

/// This represents a source file.
JSON_OBJECT_BEGIN(File)
JSON_OBJECT_PAIR_2(File
  , ID, llvm::sys::fs::UniqueID;
  , Name, std::string
  )

  File() : JSON_INIT(File, llvm::sys::fs::UniqueID{}, "") {}
  ~File() = default;

  File(const File &) = default;
  File & operator=(const File &) = default;
  File(File &&) = default;
  File & operator=(File &&) = default;
JSON_OBJECT_END(File)

/// This represents location in a source code.
JSON_OBJECT_BEGIN(Location)
JSON_OBJECT_PAIR_6(Location,
  File, llvm::sys::fs::UniqueID,
  Line, unsigned,
  Column, unsigned,
  MacroFile, llvm::sys::fs::UniqueID,
  MacroLine, unsigned,
  MacroColumn, unsigned)

  Location()
    : JSON_INIT(Location, llvm::sys::fs::UniqueID{}, 0, 0,
                llvm::sys::fs::UniqueID{}, 0, 0) {}
  ~Location() = default;

  Location(const Location &) = default;
  Location & operator=(const Location &) = default;
  Location(Location &&) = default;
  Location & operator=(Location &&) = default;
JSON_OBJECT_END(Location)
}
}

JSON_DEFAULT_TRAITS(tsar::msg::, Diagnostic)
JSON_DEFAULT_TRAITS(tsar::msg::, File)
JSON_DEFAULT_TRAITS(tsar::msg::, Location)

namespace json {
/// Specialization of JSON serialization traits for tsar::msg::Status type.
template<> struct Traits<tsar::msg::Status> {
  static bool parse(tsar::msg::Status &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::Status>(S)
        .Case("Success", tsar::msg::Status::Success)
        .Case("Done", tsar::msg::Status::Done)
        .Case("Error", tsar::msg::Status::Error)
        .Default(tsar::msg::Status::Invalid);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::msg::Status Obj) {
    JSON += '"';
    switch (Obj) {
      case tsar::msg::Status::Success: JSON += "Success"; break;
      case tsar::msg::Status::Done: JSON += "Done"; break;
      case tsar::msg::Status::Error: JSON += "Error"; break;
      default: JSON += "Invalid"; break;
    }
    JSON += '"';
}
};

/// Specialization of JSON serialization traits for tsar::msg::Analysis type.
template<> struct Traits<tsar::msg::Analysis> {
  static bool parse(tsar::msg::Analysis &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::Analysis>(S)
        .Case("Yes", tsar::msg::Analysis::Yes)
        .Case("No", tsar::msg::Analysis::No)
        .Default(tsar::msg::Analysis::Invalid);
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, tsar::msg::Analysis Obj) {
    JSON += '"';
    switch (Obj) {
      case tsar::msg::Analysis::Yes: JSON += "Yes"; break;
      case tsar::msg::Analysis::No: JSON += "No"; break;
      default: JSON += "Invalid"; break;
    }
    JSON += '"';
  }
};

/// Specialization of JSON serialization traits for llvm::Optional type.
///
/// Note that 'T' must be default constructible and copy assignable.
template<class T> struct Traits<llvm::Optional<T>> {
  static_assert(std::is_default_constructible<T>::value,
    "Underlining type must be default constructible!");
  static_assert(std::is_copy_assignable<T>::value,
    "Underlining type must be copy assignable!");
  static bool parse(llvm::Optional<T> &Dest, ::json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      if (S == "null") {
        Dest.reset();
      } else {
        T Tmp;
        Traits<T>::parse(Tmp, Lex);
        Dest = std::move(Tmp);
      }
    }
    catch (...) {
      return false;
    }
    return true;
  }
  static void unparse(String &JSON, const llvm::Optional<T> &Obj) {
    if (Obj.hasValue())
      Traits<T>::unparse(JSON, *Obj);
    else
      JSON += R"(null)";
  }
};

template <> struct Traits<llvm::sys::fs::UniqueID> {
  inline static bool parse(llvm::sys::fs::UniqueID &Dest, Lexer &Lex) {
    auto &&[Count, MaxIdx, Ok] = Parser<>::numberOfKeys(Lex);
    if (!Ok || Count != 2 || Count != MaxIdx + 1)
      return false;
    return Parser<>::traverse<Traits<llvm::sys::fs::UniqueID>>(Dest, Lex);
  }

  inline static bool parse(llvm::sys::fs::UniqueID &Dest, Lexer &Lex,
                           std::pair<Position, Position> Key) {
    Position Idx{Key.second};
    if (Key.first != 0)
      Idx = std::stoull(
          Lex.json().substr(Key.first + 1, Key.second - Key.first - 1));
    auto Device{Dest.getDevice()};
    auto File{Dest.getFile()};
    switch (Idx) {
    default:
      return false;
    case 0:
      if (!Traits<decltype(Device)>::parse(Device, Lex))
        return false;
      break;
    case 1:
      if (!Traits<decltype(File)>::parse(File, Lex))
        return false;
      break;
    }
    Dest = llvm::sys::fs::UniqueID(Device, File);
    return true;
  }

  inline static void unparse(String &JSON, llvm::sys::fs::UniqueID Obj) {
    JSON += '[';
    auto Device{Obj.getDevice()};
    auto File{Obj.getFile()};
    Traits<decltype(Device)>::unparse(JSON, Device);
    JSON += ',';
    Traits<decltype(File)>::unparse(JSON, File);
    JSON += ']';
  }
};
}
#endif//TSAR_MESSAGES_H
