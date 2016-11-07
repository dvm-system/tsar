//===---- tsar_exception.h ------- List Of Errors ---------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file contains descriptions of errors possibly arising during analysis
// of a program.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_EXCEPTION_H
#define TSAR_EXCEPTION_H

#include <exception.h>
#ifdef TSAR_CONFIG
#include "tsar_config.h"
#else
#define TSAR_FULL_NAME "Traits Static Analyzer"
#define TSAR_NAME "TSAR"
#define TSAR_STRING "TSAR"
#define TSAR_AUTHOR "Nikita A. Kataev (kaniandr@gmail.com)"
#define TSAR_URL "http://dvm-system.org/"
#endif

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

  /// Version of the analyzer.
  DESCRIPTION_FIELD(Version, TEXT("Version"), TEXT(TSAR_VERSION))

  /// Author of the analyzer.
  DESCRIPTION_FIELD(Author, TEXT("Author"), TEXT(TSAR_AUTHOR))

  /// URL of the analyzer.
  DESCRIPTION_FIELD(URL, TEXT("URL"), TEXT(TSAR_URL))

  /// List contains description of the library.
  typedef bcl::StaticMap<Title, Acronym, Version, Author> Description;

  /// List of errors possibly arising during analysis of a program.
  typedef bcl::StaticMap<ErrorList::Unsupported, ErrorList::Unclassified> Errors;

  /// Priority of errors.
  typedef Base::ErrorList::Priority ErrorPriority;

  /// Dependences from other applications.
  typedef bcl::StaticMap<Base::BCL> Applications;

  /// Stub required to add a description to a static list of applications.
  typedef Utility::Null ValueType;
};

/// This encapsulated base errors arising in library for program analysis.
typedef Base::Exception<TSAR> TSARException;
}
#endif//TSAR_EXCEPTION_H
