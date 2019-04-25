//===----- tsar_utility.h - Utility Methods and Classes ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_UTILITY_H
#define TSAR_UTILITY_H

#include "DIMemoryLocation.h"
#include "tsar_config.h"
#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Type.h>
#include <tuple>
#include <bcl/convertible_pair.h>
#include <bcl/tagged.h>

#if !defined LLVM_RELEASE_BUILD && defined TSAR_ENABLE_LLVM_DUMP
/// Use this macro if dump() is called for LLVM objects. Otherwise, link-time
/// errors occur if configuration of LLVM build is not Debug.
# define TSAR_LLVM_DUMP(X) do { X; } while (false)
#else
/// Use this macro if dump() is called for LLVM objects. Otherwise, link-time
/// errors occur if configuration of LLVM build is not Debug.
# define TSAR_LLVM_DUMP(X)
#endif

namespace llvm {
class BasicBlock;
class DIGlobalVariable;
class DominatorTree;
class GlobalVariable;
class DILocalVariable;
class DIVariable;
class DISubprogram;
class AllocaInst;
class Instruction;
class Use;
class Module;
}

namespace tsar {
/// This tag provides access to low-level representation of matched entities.
struct IR {};

/// This tag provides access to source-level representation of matched entities.
struct AST {};

/// This tag provides access to metadata-level representation of matched entities.
struct MD {};

/// This tag is used to implement hierarchy of nodes.
struct Hierarchy {};

/// This tag is used to implement a sequence of memory locations which may alias.
struct Alias {};

/// This tag is used to implement a sequence of sibling nodes.
struct Sibling {};

/// This tag is used to implement a sequence of nodes which is treated as a pool.
struct Pool {};

/// This tag is used to provide access to a set of traits of a region.
struct Region {};

/// Loop identifier.
using LoopID = llvm::MDNode *;

/// Merges elements from a specified range using a specified delimiter, put
/// result to a specified buffer and returns reference to it.
template<class ItrT>
llvm::StringRef join(ItrT I, ItrT EI, llvm::StringRef Delimiter,
    llvm::SmallVectorImpl<char> &Out) {
  llvm::raw_svector_ostream OS(Out);
  OS << *I;
  for (++I; I != EI; ++I)
    OS <<  Delimiter << *I;
  return llvm::StringRef(Out.data(), Out.size());
}

/// Merges elements from a specified range using a specified delimiter.
template<class ItrT>
std::string join(ItrT I, ItrT EI, llvm::StringRef Delimiter) {
  llvm::SmallString<256> Out;
  return join(I, EI, Delimiter, Out);
}

/// \brief Splits a specified string into tokens according to a specified
/// pattern.
///
/// See std::regex for syntax of a pattern string. If there are successful
/// sub-matches, each of them becomes a token. Otherwise, the whole matching
/// sequence becomes a token.
/// For example, in case of `(struct)\s` pattern and `struct A` string, `struct`
/// is a token (the whole matching sequence is `struct `). However, in case of
/// `struct\s` pattern `strcut ` is a token.
///
/// \attention Note, that this function does not allocate memory for the result.
/// This means that result vector contains references to substrings of
/// a specified string `Str` and the vector will be valid only while
/// `Str` is valid.
std::vector<llvm::StringRef> tokenize(
  llvm::StringRef Str, llvm::StringRef Pattern);

/// \brief An iterator type that allows iterating over the pointers via some
/// other iterator.
///
/// The typical usage of this is to expose a type that iterates over Ts, but
/// which is implemented with some iterator over some wrapper of T*s
/// \code
///   typedef wrapped_pointer_iterator<
///     std::vector<std::unique_ptr<T>>::iterator> iterator;
/// \endcode
template <class WrappedIteratorT,
          class T = decltype(&**std::declval<WrappedIteratorT>())>
class wrapped_pointer_iterator : public llvm::iterator_adaptor_base<
  wrapped_pointer_iterator<WrappedIteratorT>, WrappedIteratorT,
    class std::iterator_traits<WrappedIteratorT>::iterator_category, T> {
public:
  wrapped_pointer_iterator() = default;
  template<class ItrT> wrapped_pointer_iterator(ItrT &&I) :
    wrapped_pointer_iterator::iterator_adaptor_base(std::forward<ItrT &&>(I)) {}

  T & operator*() { return Ptr = &**this->I; }
  const T & operator*() const { return Ptr = &**this->I; }

private:
  mutable T Ptr;
};

/// Returns argument with a specified number or nullptr.
llvm::Argument * getArgument(llvm::Function &F, std::size_t ArgNo);

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

/// Returns number of dimensions in a specified type or 0 if it is not an array.
inline unsigned dimensionsNum(const llvm::Type *Ty) {
  unsigned Dims = 0;
  for (; Ty->isArrayTy(); Ty = Ty->getArrayElementType(), ++Dims);
  return Dims;
}

/// Returns number of dimensions and elements in a specified type and type of
/// innermost array element. If `Ty` is not an array type this function returns
/// 0,1, Ty.
inline std::tuple<unsigned, uint64_t, llvm::Type *>
arraySize(llvm::Type *Ty) {
  assert(Ty && "Type must not be null!");
  unsigned Dims = 0;
  uint64_t NumElements = 1;
  for (; Ty->isArrayTy(); Ty = Ty->getArrayElementType(), ++Dims)
    NumElements *= llvm::cast<llvm::ArrayType>(Ty)->getArrayNumElements();
  return std::make_tuple(Dims, NumElements, Ty);
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
inline llvm::DITypeRef arrayElementDIType(llvm::DITypeRef &DITy) {
  using namespace llvm;
  auto ElTy = stripDIType(DITy);
  if (!ElTy.resolve() || !isa<llvm::DIDerivedType>(ElTy))
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
/// returns 'true' if a specified type is one of thes types and it has not bee
/// accurately generated.
///
/// TODO (kaniandr@gmail.com): may be we should use other way to distinguish
/// such types. How LLVM uses 'artificial' flag on types?
inline bool isStubType(llvm::DITypeRef DITy) {
  auto Ty = DITy.resolve();
  return !Ty || (Ty->isArtificial() && Ty->getName() == "sapfor.type");
}

/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator==(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  if (LHS.size() != RHS.size())
    return false;
  for (PtrType V : LHS)
    if (RHS.count(V) == 0)
      return false;
  return true;
}

/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator!=(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  return !(LHS == RHS);
}

namespace detail {
/// Applies a specified function object to each loop in a loop tree.
template<class Function>
void for_each_loop(llvm::LoopInfo::reverse_iterator ReverseI,
  llvm::LoopInfo::reverse_iterator ReverseEI,
  Function F) {
  for (; ReverseI != ReverseEI; ++ReverseI) {
    F(*ReverseI);
    for_each_loop((*ReverseI)->rbegin(), (*ReverseI)->rend(), F);
  }
}
}

/// Applies a specified function object to each loop in a loop tree.
template<class Function>
Function for_each_loop(const llvm::LoopInfo &LI, Function F) {
  detail::for_each_loop(LI.rbegin(), LI.rend(), F);
  return std::move(F);
}

/// \brief Clone chain of instruction.
///
/// All clones instruction will be pushed to the `CloneList`. If some of
/// instructions can not be cloned the `CloneList` will not be updated.
/// Clones will not be inserted into IR. The lowest clone sill be the first
/// instruction which is pushed into the `CloneList`.
///
/// If `BoundInst` and `DT` is `nullptr` the full use-def chain will be cloned
/// (except Phi-nodes and alloca instructions). In case of Phi-nodes this method
/// returns false (cloning is impossible). The lowest instruction in the cloned
/// chain is `From`. If `BoundInst` and `DT` is specified instructions which
/// dominate BoundInst will not be cloned.
///
/// \return `true` on success, if instructions should not be cloned this
/// function also returns `true`.
bool cloneChain(llvm::Instruction *From,
  llvm::SmallVectorImpl<llvm::Instruction *> &CloneList,
  llvm::Instruction *BoundInst = nullptr, llvm::DominatorTree *DT = nullptr);

/// \brief Traverses chains of operands of `From` instruction and performs a
/// a search for operands which do not dominate a specified `BoundInstr`.
///
/// This method if helpful when some instruction is cloned. Insertion
/// of a clone into IR should be performed accurately. Because operands must be
/// calculated before their uses. This functions can be used to determine
/// operands that should be fixed (for example, also cloned).
///
/// \return `true` if `From` does not dominate `BoundInst` (in this case NotDom
/// will be empty), otherwise return `false` (`NotDom` will contain all
/// instructions which have been found).
bool findNotDom(llvm::Instruction *From,
  llvm::Instruction *BoundInst, llvm::DominatorTree *DT,
  llvm::SmallVectorImpl<llvm::Use *> &NotDom);

/// This function strips off any GEP address adjustments and pointer casts from
/// the specified value while it is possible or until the object with attached
/// metadata describing a value will be found. The second returned value is
/// true if a returned object has attached metadata.
std::pair<llvm::Value *, bool> GetUnderlyingObjectWithMetadata(llvm::Value *V,
  const llvm::DataLayout &DL);

/// Returns a meta information for function or nullptr;
llvm::DISubprogram * findMetadata(const llvm::Function *F);

/// Find meta information available a global variable.
/// Returns 'true' if at least one valid metadata has been found.
bool findGlobalMetadata(const llvm::GlobalVariable *Var,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs);

/// \brief Checks that two fragments of a variable may overlap.
///
/// Two fragments of zero size may not overlap. Note that there is no reason
/// to invoke this functions for fragments of different variables. Complex
/// expressions which contains elements other then dwarf::DW_OP_LLVM_fragment
/// does not analyzed accurately. In this case overlapping is conservatively
/// assumed.
bool mayAliasFragments(
  const llvm::DIExpression &LHS, const llvm::DIExpression &RHS);

/// Specify kind of metadata which should be found.
enum class MDSearch { AddressOfVariable, ValueOfVariable, Any };

/// \brief Returns a meta information for a specified value or nullptr.
///
/// \param [in] V Analyzed value.
/// \param [in] DT If it is specified then llvm.dbg.value will be analyzed if
/// necessary. Otherwise llvm.dbg.declare, llvm.dbg.address and global
/// variables will be analyzed only.
/// \param [in] MDS Kind of metadata which should be found. If AddressOfVariable
/// is used then `V` is an address of a variable and dbg.declare, dbg.address
/// should be found only. If ValueOfVariable is used then `V` is a value of a
/// variable and dbg.value should be found only.
/// \param [out] DILocs This will contain all metadata-level locations which are
/// associated with a specified value. For this reason llvm.dbg. ...
/// intrinsics will be analyzed. Intrinsics which dominates all
/// uses of `V` will be only considered. The condition mentioned bellow is also
/// checked. Let us consider some llvm.dbg.value `I` which dominates all uses of
/// `V` and associates a metadata-level `DILoc` with `V`. Paths from `I` to each
/// use of V will be checked. There should be no other intrinsics which
/// associates `DILoc` with some other value on these paths.
/// \return A variable from `DILocs`, llvm.dbg.value for this
/// variable dominates llvm.dbg.value for other variables from `DILocs`. If
/// there is no such variable, None is returned.
llvm::Optional<DIMemoryLocation> findMetadata(const llvm::Value * V,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs,
  const llvm::DominatorTree *DT = nullptr, MDSearch MDS = MDSearch::Any);

/// Return meta information for a specified value or None.
///
/// This function is similar to previously defined function findMetadata(),
/// however it performs only MDSearch::ValueOfVariable search. The main
/// difference is it consider a specified users 'Users' of a specified value `V'.
/// A general findMetadata() function consider all users of `V` instead.
llvm::Optional<DIMemoryLocation> findMetadata(const llvm::Value *V,
  llvm::ArrayRef<llvm::Instruction *> Users, const llvm::DominatorTree &DT,
  llvm::SmallVectorImpl<DIMemoryLocation> &DILocs);

/// \brief This is an implementation of detail::DenseMapPair which supports
/// access to a first and second value via tag of a type (Pair.get<Tag>()).
///
/// Let us consider an example:
/// \code
///   struct Foo {};
///   typedef llvm::DenseMap<KT *, VT *, llvm::DenseMapInfo<KT *>,
///     tsar::TaggedDenseMapPair<
///       bcl::tagged<KT *, KT>, bcl::tagged<VT *, Foo>>> Map;
///   Map M;
///   auto I = M.begin();
///   I->get<KT>() // equivalent to I->first
///   I->get<Foo>() // equivalent to I->second
/// \endcode
template<class TaggedKey, class TaggedValue>
struct TaggedDenseMapPair : public bcl::tagged_pair<TaggedKey, TaggedValue> {
  typedef typename TaggedKey::type KeyT;
  typedef typename TaggedValue::type ValueT;
  KeyT &getFirst() { return std::pair<KeyT, ValueT>::first; }
  const KeyT &getFirst() const { return std::pair<KeyT, ValueT>::first; }
  ValueT &getSecond() { return std::pair<KeyT, ValueT>::second; }
  const ValueT &getSecond() const { return std::pair<KeyT, ValueT>::second; }
};

/// \brief This is an implementation of detail::DenseMapPair which supports
/// access to all values via tag of a type.
///
/// It is possible to store in a map some tuple and access key and values from
/// this tuple via tag of a type.
/// Let us consider an example:
/// \code
///   struct Foo {};
///   struct Bar {};
///   typedef llvm::DenseMap<KT *,
///     std::tuple<VT1 *, VT2 *>,
///     llvm::DenseMapInfo<KT *>,
///     tsar::TaggedDenseMapTuple<
///       bcl::tagged<KT *, KT>,
///       bcl::tagged<VT1 *, Foo>>
///       bcl::tagged<VT2 *, Bar>>> Map;
///   Map M;
///   Map.insert(std::make_pair(K, std::make_tuple(V1, V2));
///   auto I = M.begin();
///   I->get<KT>() // equivalent to I->first
///   I->get<Foo>() // equivalent to I->second.get<0>();
///   I->get<Bar>() // equivalent to I->second.get<1>();
/// \endcode
template<class TaggedKey, class... TaggedValue>
struct TaggedDenseMapTuple :
    public std::pair<
      typename TaggedKey::type, bcl::tagged_tuple<TaggedValue...>> {
  typedef std::pair<
    typename TaggedKey::type, bcl::tagged_tuple<TaggedValue...>> BaseT;
  typedef typename TaggedKey::type KeyT;
  typedef std::tuple<typename TaggedValue::type...> ValueT;

  KeyT &getFirst() { return BaseT::first; }
  const KeyT &getFirst() const { return BaseT::first; }
  ValueT &getSecond() { return BaseT::second; }
  const ValueT &getSecond() const { return BaseT::second; }

  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<
        bcl::get_tagged<Tag, TaggedKey, TaggedValue...>>::value>::type>
  bcl::get_tagged_t<Tag, TaggedKey, TaggedValue...> &
  get() noexcept {
    return get<Tag>(bcl::tags::is_alias<Tag, TaggedKey>());
  }

  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<
        bcl::get_tagged<Tag, TaggedKey, TaggedValue...>>::value>::type>
  const bcl::get_tagged_t<Tag, TaggedKey, TaggedValue...> &
  get() const noexcept {
    return get<Tag>(bcl::tags::is_alias<Tag, TaggedKey>());
  }
private:
  template<class Tag>
  KeyT & get(std::true_type) noexcept { return BaseT::first; }

  template<class Tag>
  const KeyT & get(std::true_type) const noexcept { return BaseT::first; }

  template<class Tag> bcl::get_tagged_t<Tag, TaggedValue...> &
  get(std::false_type) noexcept {
    return BaseT::second.template get<Tag>();
  }

  template<class Tag> const bcl::get_tagged_t<Tag, TaggedValue...> &
  get(std::false_type) const noexcept {
    return BaseT::second.template get<Tag>();
  }
};
}

namespace llvm {
/// This is a specialization of llvm::DenseMapInfo for bcl::convertible_pair.
template<class FirstTy, class SecondTy>
class DenseMapInfo<bcl::convertible_pair<FirstTy, SecondTy>> :
  public DenseMapInfo<std::pair<FirstTy, SecondTy>> {};

/// This allows passes to create callbacks that run when the underlying Value
/// has RAUW called on it or is destroyed.
struct  CallbackVHFactory {
  /// Invokes when someone wants to create callbacks for a specified value.
  virtual void create(llvm::Value *V) = 0;

  /// Invokes when someone wants to delete all created callbacks, for example
  /// in Pass::releaseMemory() method.
  virtual void destroy() = 0;

  /// Virtual destructor.
  virtual ~CallbackVHFactory() {}
};
}
#endif//TSAR_UTILITY_H
