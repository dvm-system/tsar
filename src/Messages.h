//===------ Messages.h ------ Traits Static Analyzer ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This defines general messages for server-client interaction, where server
// is TSAR. This also defines some utility entities to simplify definition of
// a new message.
//
// Each message is a JSON string which is parsed and unparsed with JSON String
// Serializer from Json.h file. High-level message representation is a class
// inherited from json::Object and bcl::StaticMap. To simplify a new message
// definition this file proposes macros MSG_FIELD_TYPE, MSG_FIELD, MSG_NAME.
//
// Let us consider usage of this macros on example of msg::Diagnostic message.
//
// At first structure of message should be declared:
//  namespace tsar {
//  namespace msg {
//  namespace detail {
//  struct Diagnostic {
//    MSG_FIELD_TYPE(Status, msg::Status)
//    MSG_FIELD_TYPE(Error, std::vector<std::string>)
//    MSG_FIELD_TYPE(Warning, std::vector<std::string>)
//    MSG_FIELD_TYPE(Terminal, std::string)
//    typedef bcl::StaticMap<Status, Error, Warning, Terminal> Base;
//  };
//  }
//
// This message contains field with names Status, Error, Warning, Terminal
// (the first parameter of MSG_FIELD_TYPE macro) and types msg::Status
// (enum declared in this file), vector, vector and string correspondingly
// (the second parameter of MSG_FIELD_TYPE). This structure is placed in
// detail namespace because it is internal information about message and
// should not be accessed directly by the user.
//
//  class Diagnostic :
//    public json::Object, public msg::detail::Diagnostic::Base{
//  public:
//    MSG_NAME(Diagnostic)
//    MSG_FIELD(Diagnostic, Status)
//    MSG_FIELD(Diagnostic, Error)
//    MSG_FIELD(Diagnostic, Warning)
//    MSG_FIELD(Diagnostic, Terminal)
//
//    Diagnostic(msg::Status S) : json::Object(name()), StaticMap(S) {}
//
//    Diagnostic(const Diagnostic &) = default;
//    Diagnostic & operator=(const Diagnostic &) = default;
//    Diagnostic(Diagnostic &&) = default;
//    Diagnostic & operator=(Diagnostic &&) = default;
//    ...
//  };
//  }
//  }
//
// Now me declare the main class for diagnostic representation. MSG_NAME macro
// specified identifier of this message and MSG_FIELD macro proposes a way
// to access previously defined fields (for example use Diag[msg::Status] to
// read/write Status filed of Diagnostic message stored in Diag object).
//
// At least one constructor must be defined, it should initialize the base class
// json::Object with object name (json::Object(name()). The name() method
// is defined by MSG_NAME macro.
//
// The class json::Object has virtual destructor so default copy constructors
// and assignment operators will not be defined for msg::Diagnostic by the
// compiler. But it is still possible to use '= default' construction to force
// such definition.
//
// The last one step is definition of parsing rules for JSON serializatoin.
//
//  template<> struct json::Traits<tsar::msg::Diagnostic> :
//    public json::Traits<tsar::msg::detail::Diagnostic::Base> {};
//
// Note, that the type of Status file is user defined type so JSON serialization
// rules must be also psoposed for this type (see detail in this file).
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_MESSAGES_H
#define TSAR_MESSAGES_H

#include <llvm/ADT/StringSwitch.h>
#include <array>
#include <vector>
#include <string>
#include <cell.h>
#include <Diagnostic.h>
#include <Json.h>

/// Specifies identifier of a message (provides the name() method).
#define MSG_NAME(Name) \
static inline const std::string & \
name( ) { static const std::string N(#Name); return N;}

/// Specifies identifiers and types of message fields.
#define MSG_FIELD_TYPE(Field, Type) \
struct Field { MSG_NAME(Field) typedef Type ValueType; };

/// Specifies a way to access message fields.
#define MSG_FIELD(Msg, Field) \
static constexpr detail::##Msg##::##Field Field = detail::##Msg##::##Field();

namespace tsar {
namespace msg {
/// Represents general static information about header files.
struct HeaderFile {
  MSG_NAME(header)
  static const std::array<char *, 2> & extensions() {
    static std::array<char *, 2> Exts{ { ".h", ".hpp" } };
    return Exts;
  }
};

/// Represents general static information about source files.
struct SourceFile {
  MSG_NAME(source)
  static const std::array<char *, 3> & extensions() {
    static std::array<char *, 3> Exts{ {".c", ".cpp", ".cxx"} };
    return Exts;
  }
};

/// Represents general static information about unknown files.
struct OtherFile {
  MSG_NAME(other)
};

/// \brief This defines Status type of field for some messages.
///
/// For example it can be used to provide status of some operation execution.
/// Appropriate JSON serialization rules are defined bellow.
enum class Status : short {
  First = 0,
  Success = First,
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

namespace detail {
struct Diagnostic {
  MSG_FIELD_TYPE(Status, msg::Status)
  MSG_FIELD_TYPE(Error, std::vector<std::string>)
  MSG_FIELD_TYPE(Warning, std::vector<std::string>)
  MSG_FIELD_TYPE(Terminal, std::string)
  typedef bcl::StaticMap<Status, Error, Warning, Terminal> Base;
};
}

/// \brief This message provide diagnostic for analysis execution.
///
/// Terminal filed is used to provide data from stdout or/and stderr.
class Diagnostic :
  public json::Object, public msg::detail::Diagnostic::Base {
public:
  MSG_NAME(Diagnostic)
  MSG_FIELD(Diagnostic, Status)
  MSG_FIELD(Diagnostic, Error)
  MSG_FIELD(Diagnostic, Warning)
  MSG_FIELD(Diagnostic, Terminal)

  /// Creates new diagnostic of a specified status.
  Diagnostic(msg::Status S) : json::Object(name()), StaticMap(S) {}

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
};
}
}

/// Specialization of JSON serialization traits for tsar::msg::Status type.
template<> struct json::Traits<tsar::msg::Status> {
  static bool parse(tsar::msg::Status &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::Status>(Lex.json())
        .Case("Success", tsar::msg::Status::Success)
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
      case tsar::msg::Status::Error: JSON += "Error"; break;
      default: JSON += "Invalid"; break;
    }
    JSON += '"';
}
};

/// Specialization of JSON serialization traits for tsar::msg::Analysis type.
template<> struct json::Traits<tsar::msg::Analysis> {
  static bool parse(tsar::msg::Analysis &Dest, json::Lexer &Lex) noexcept {
    try {
      auto Value = Lex.discardQuote();
      auto S = Lex.json().substr(Value.first, Value.second - Value.first + 1);
      Dest = llvm::StringSwitch<tsar::msg::Analysis>(Lex.json())
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

template<> struct json::Traits<tsar::msg::Diagnostic> :
  public json::Traits<tsar::msg::detail::Diagnostic::Base> {};
#endif//TSAR_MESSAGES_H
