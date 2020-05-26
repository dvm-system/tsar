//===----- MetadataUtils.h - Utils for exploring metadata -------*- C++ -*-===//
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
// This file defines helpful functions to access metadata.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_METADATA_UTILS_H
#define TSAR_SUPPORT_METADATA_UTILS_H

#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/Optional.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/DebugInfoMetadata.h>

namespace tsar {
/// Returns a language for a specified function.
inline llvm::Optional<unsigned> getLanguage(const llvm::Function &F) {
  if (auto *MD = F.getSubprogram())
    if (auto CU = MD->getUnit())
      return CU->getSourceLanguage();
  return llvm::None;
}

/// Returns a language for a specified variable.
llvm::Optional<unsigned> getLanguage(const llvm::DIVariable &DIVar);

/// Returns true if a specified language is C language.
inline bool isC(unsigned DWLang) noexcept {
  using namespace llvm;
  switch (DWLang) {
  case dwarf::DW_LANG_C:
  case dwarf::DW_LANG_C89:
  case dwarf::DW_LANG_C99:
  case dwarf::DW_LANG_C11:
  case dwarf::DW_LANG_ObjC:
    return true;
  default:
    return false;
  }
}

/// Returns true if a specified language is C++ language.
inline bool isCXX(unsigned DWLang) noexcept {
  using namespace llvm;
  switch (DWLang) {
  case dwarf::DW_LANG_C_plus_plus:
  case dwarf::DW_LANG_C_plus_plus_03:
  case dwarf::DW_LANG_C_plus_plus_11:
  case dwarf::DW_LANG_C_plus_plus_14:
    return true;
  default:
    return false;
  }
}

/// Returns true if a specified language is Fortran language.
inline bool isFortran(unsigned DWLang) noexcept {
  using namespace llvm;
  switch (DWLang) {
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    return true;
  default:
    return false;
  }
}

/// Return `true` in case of forward direction of dimensions of arrays in memory.
////
/// For example, `true` in case of C and `false` in case of Fortran.
inline bool isForwardDim(unsigned DWLang) noexcept {
  return isC(DWLang) || isCXX(DWLang);
}

/// Returns size of type, in address units, type must not be null.
inline uint64_t getSize(llvm::DIType *Ty) {
  assert(Ty && "Type must not be null!");
  return (Ty->getSizeInBits() + 7) / 8;
}

/// Returns size of type, in address units, type must not be null.
inline uint64_t getSize(llvm::DITypeRef DITy) {
  return getSize(DITy.resolve());
}

/// Return number of elements in a subrange in address units if size is constant.
llvm::Optional<uint64_t> getConstantCount(const llvm::DISubrange &Range);

/// \brief Strips types that do not change representation of appropriate
/// expression in a source language.
///
/// For example, const int and int & will be stripped to int, typedef will be
/// also stripped.
inline llvm::DITypeRef stripDIType(llvm::DITypeRef DITy) {
  using namespace llvm;
  if (!DITy.resolve() || !isa<llvm::DIDerivedType>(DITy))
    return DITy;
  auto DIDTy = cast<DIDerivedType>(DITy);
  switch (DIDTy->getTag()) {
  case dwarf::DW_TAG_typedef:
  case dwarf::DW_TAG_const_type:
  case dwarf::DW_TAG_reference_type:
  case dwarf::DW_TAG_volatile_type:
  case dwarf::DW_TAG_restrict_type:
  case dwarf::DW_TAG_rvalue_reference_type:
    return stripDIType(DIDTy->getBaseType());
  }
  return DITy;
}

/// Returns type of an array element or nullptr if type is unknown.
inline llvm::DITypeRef arrayElementDIType(llvm::DITypeRef DITy) {
  using namespace llvm;
  auto ElTy = stripDIType(DITy);
  if (!ElTy.resolve())
    return DITypeRef();
  if (ElTy.resolve()->getTag() != dwarf::DW_TAG_pointer_type &&
      ElTy.resolve()->getTag() != dwarf::DW_TAG_array_type)
    return DITypeRef();
  if (ElTy.resolve()->getTag() == dwarf::DW_TAG_pointer_type)
    ElTy = cast<DIDerivedType>(ElTy)->getBaseType().resolve();
  if (ElTy.resolve()->getTag() == dwarf::DW_TAG_array_type)
    ElTy = cast<DICompositeType>(ElTy)->getBaseType().resolve();
  return stripDIType(ElTy);
}

/// If we have missing or conflicting AAInfo, return 'true'.
inline bool isAAInfoCorrupted(llvm::AAMDNodes AAInfo) {
  return (AAInfo == llvm::DenseMapInfo<llvm::AAMDNodes>::getEmptyKey() ||
    AAInfo == llvm::DenseMapInfo<llvm::AAMDNodes>::getTombstoneKey());
}

/// If we have missing or conflicting AAInfo, return null.
inline llvm::AAMDNodes sanitizeAAInfo(llvm::AAMDNodes AAInfo) {
  if (isAAInfoCorrupted(AAInfo))
    return llvm::AAMDNodes();
  return AAInfo;
}

/// Additional types may be necessary for metadata-level analysis. This function
/// returns 'true' if a specified type is one of these types and it has not been
/// accurately generated.
///
/// TODO (kaniandr@gmail.com): may be we should use other way to distinguish
/// such types. How LLVM uses 'artificial' flag on types?
inline bool isStubType(llvm::DITypeRef DITy) {
  auto Ty = DITy.resolve();
  return !Ty || (Ty->isArtificial() && Ty->getName() == "sapfor.type");
}

/// Additional variables may be necessary for metadata-level analysis.
/// This function returns 'true' if a specified variable is one of these
/// variables and it has not been accurately generated.
///
/// TODO (kaniandr@gmail.com): may be we should use other way to distinguish
/// such types. How LLVM uses 'artificial' flag on variables?
inline bool isStubVariable(llvm::DIVariable &DIVar) {
  return llvm::isa<llvm::DILocalVariable>(DIVar) &&
         llvm::cast<llvm::DILocalVariable>(DIVar).isArtificial();
}
}
#endif//TSAR_SUPPORT_METADATA_UTILS_H
