//===---- tsar_exception.h - List of TSAR errors ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains descriptions of errors possibly arising
// during analysis of a program.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_EXCEPTION_H
#define TSAR_EXCEPTION_H

#include <exception.h>
#include "tsar_config.h"

namespace tsar {
namespace ErrorList {
  /// An error indicates that a feature is not implemented yet.
  typedef Base::ErrorList::Unsupported Unsupported;

  /// This is unclassified error.
  typedef Base::ErrorList::Unclassified Unclassified;
}

/// This is a description of Traits Static Analyzer.
struct TSAR {
  /// Name of the library.
  DESCRIPTION_FIELD(Title, TEXT("Title"), TEXT(TSAR_FULL_NAME))

  /// Abbreviation of the name.
  DESCRIPTION_FIELD(Acronym, TEXT("Acronym"), TEXT(TSAR_NAME))

  /// Version of the library.
  DESCRIPTION_FIELD(Version, TEXT("Version"), TEXT(TSAR_VERSION))

  /// Author of the library.
  DESCRIPTION_FIELD(Author, TEXT("Author"), TEXT(TSAR_AUTHOR))

  /// List contains description of the library.
  typedef CELL_COLL_4(Title, Acronym, Version, Author) Description;

  /// List of errors possibly arising during analysis of a program.
  typedef CELL_COLL_2(ErrorList::Unsupported, ErrorList::Unclassified) Errors;

  /// Priority of errors.
  typedef Base::ErrorList::Priority ErrorPriority;

  /// Dependences from other applications.
  typedef CELL_COLL_1(Base::BCL) Applications;

  /// Stub required to add a description to a static list of applications.
  typedef Utility::Null ValueType;
};

/// This encapsulated base errors arising in library for program analysis.
typedef Base::Exception<TSAR> TSARxception;
}
#endif//TSAR_EXCEPTION_H
