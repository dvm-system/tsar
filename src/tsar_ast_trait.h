//===--- tsar_trait.h ------ Analyzable AST Traits --------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines traits which could be recognized by the analyzer at AST
// level.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_AST_TRAIT_H
#define TSAR_AST_TRAIT_H

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/STLExtras.h>
#include <bcl/trait.h>
#include <bcl/utility.h>

namespace clang {
class VarDecl;
}

namespace tsar {
/// Declaration of a trait recognized by analyzer at AST level.
#define TSAR_TRAIT_DECL(name_, string_) \
struct name_ { \
  static llvm::StringRef toString() { \
    static std::string Str(string_); \
    return Str; \
  } \
};

namespace trait {
namespace ast {
TSAR_TRAIT_DECL(AddressAccess, "address access")
TSAR_TRAIT_DECL(NoAccess, "no access")
TSAR_TRAIT_DECL(Shared, "shared")
TSAR_TRAIT_DECL(Private, "private")
TSAR_TRAIT_DECL(FirstPrivate, "first private")
TSAR_TRAIT_DECL(SecondToLastPrivate, "second to last private")
TSAR_TRAIT_DECL(LastPrivate, "last private")
TSAR_TRAIT_DECL(DynamicPrivate, "dynamic private")
TSAR_TRAIT_DECL(Reduction, "reduction")
TSAR_TRAIT_DECL(Dependency, "dependency")
TSAR_TRAIT_DECL(Induction, "induction")
}
}

#undef TSAR_TRAIT_DECL

/// \brief This represents list of traits for a variable which can be
/// recognized by analyzer.
///
/// \copydetail tsar::DependencyDescriptor
using ASTDependencyDescriptor = bcl::TraitDescriptor<trait::ast::AddressAccess,
  bcl::TraitAlternative<trait::ast::NoAccess, trait::ast::Shared,
  trait::ast::Private, trait::ast::Reduction, trait::ast::Induction,
  trait::ast::Dependency,
  bcl::TraitUnion<trait::ast::LastPrivate, trait::ast::FirstPrivate>,
  bcl::TraitUnion<trait::ast::SecondToLastPrivate, trait::ast::FirstPrivate>,
  bcl::TraitUnion<trait::ast::DynamicPrivate, trait::ast::FirstPrivate>>>;

/// \brief This is a set of traits for a variable.
class ASTLocationTraitSet : public bcl::TraitSet<
  ASTDependencyDescriptor, llvm::DenseMap<bcl::TraitKey, void *>> {
  /// Base class.
  using Base = bcl::TraitSet<
    ASTDependencyDescriptor, llvm::DenseMap<bcl::TraitKey, void *>>;

public:
  /// \brief Creates set of traits.
  ASTLocationTraitSet(clang::VarDecl *Loc, ASTDependencyDescriptor Dptr) :
    Base(Dptr), mLoc(Loc) {
    assert(Loc && "Location must not be null!");
  }

  /// Returns memory location.
  clang::VarDecl * memory() noexcept { return mLoc; }

  /// Returns memory location.
  const clang::VarDecl * memory() const noexcept { return mLoc; }

private:
  clang::VarDecl *mLoc;
};

/// This is a set of different traits suitable for loops.
class ASTDependencySet {
  typedef llvm::DenseMap<const clang::VarDecl *,
    std::unique_ptr<ASTLocationTraitSet>> LocationTraitMap;
public:
  /// This class used to iterate over traits of different memory locations.
  class iterator :
    public std::iterator<std::forward_iterator_tag, ASTLocationTraitSet> {
  public:
    explicit iterator(const LocationTraitMap::const_iterator &I) :
      mCurrItr(I) {}
    explicit iterator(LocationTraitMap::const_iterator &&I) :
      mCurrItr(std::move(I)) {}
    ASTLocationTraitSet & operator*() const { return *mCurrItr->second; }
    ASTLocationTraitSet * operator->() const { return &operator*(); }
    bool operator==(const iterator &RHS) const {
      return mCurrItr == RHS.mCurrItr;
    }
    bool operator!=(const iterator &RHS) const { return !operator==(RHS); }
    iterator & operator++() { ++mCurrItr; return *this; }
    iterator operator++(int) { iterator Tmp = *this; ++*this; return Tmp; }
  private:
    LocationTraitMap::const_iterator mCurrItr;
  };

  /// This type used to iterate over traits of different variables.
  typedef iterator const_iterator;

  /// \brief Finds traits of a variable.
  iterator find(const clang::VarDecl *Loc) const {
    return iterator(mTraits.find(Loc));
  }

  /// \brief Insert traits of a variable.
  std::pair<iterator, bool> insert(
    clang::VarDecl * Loc, ASTDependencyDescriptor Dptr) {
    auto Pair = mTraits.insert(
      std::make_pair(Loc, llvm::make_unique<ASTLocationTraitSet>(Loc, Dptr)));
    return std::make_pair(iterator(std::move(Pair.first)), Pair.second);
  }

  /// Erase traits of a variable.
  bool erase(const clang::VarDecl *Loc) {
    return mTraits.erase(Loc);
  }

  /// Erase traits of all variables.
  void clear() { mTraits.clear(); }

  /// Returns true if there are no variables with known traits.
  bool empty() const { return mTraits.empty(); }

  /// Returns number of variables with known traits.
  unsigned size() const { return mTraits.size(); }

  /// Returns iterator that points to the beginning of the traits list.
  iterator begin() const { return iterator(mTraits.begin()); }

  /// Returns iterator that points to the ending of the traits list.
  iterator end() const { return iterator(mTraits.end()); }

private:
  LocationTraitMap mTraits;
};

/// This attribute is associated with DependencySet and
/// can be added to a node in a data-flow graph.
BASE_ATTR_DEF(ASTDependencyAttr, ASTDependencySet)

}
#endif//TSAR_AST_TRAIT_H
