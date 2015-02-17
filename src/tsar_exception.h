/*! \file
    \brief This contains descriptions of errors possibly arising during analysis of a program.
*/
#ifndef TSAR_EXCEPTION_H
#define TSAR_EXCEPTION_H

#include <exception.h>

namespace Analyzer
{
    namespace ErrorList
    {
        typedef Base::ErrorList::Unsupported Unsupported; //!< An error indicates that a feature is not implemented yet.
        typedef Base::ErrorList::Unclassified Unclassified; //!< This is unclassified error.
    }

    //! This is a description of Traits Static Aalyzer.
    struct TSAR
    {
        //! Name of the library.
        DESCRIPTION_FIELD(Title, TEXT("Title"), TEXT("Traits Static Analyzer"))

        //! Abbreviation of the name.
        DESCRIPTION_FIELD(Acronym, TEXT("Acronym"), TEXT("TSAR"))

        //! Version of the library.
        DESCRIPTION_FIELD(Version, TEXT("Version"), TEXT("0.1 (13.12.2014)"))

        //! Author of the library.
        DESCRIPTION_FIELD(Author, TEXT("Author"), TEXT("Nikita A. Kataev, kataev_nik@mail.ru"))

        //! List contains description of the library.
        typedef CELL_COLL_4(Title, Acronym, Version, Author) Description;

        //! List of errors possibly arising during analysis of a program.
        typedef CELL_COLL_2(ErrorList::Unsupported, ErrorList::Unclassified) Errors;

        //! Priority of errors.
        typedef Base::ErrorList::Priority ErrorPriority;

        //! Dependences from other applications.
        typedef CELL_COLL_1(Base::BCL) Applications;

        //! Stub required to add a description to a static list of applications.
        typedef Utility::Null ValueType;
    };

    //! This encapsulated base errors arising in library for program analysis.
    typedef Base::Exception<TSAR> TSARxception;
}
#endif//TSAR_EXCEPTION_H
