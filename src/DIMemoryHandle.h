//===- DIMemoryHandle.h - DIMemory Smart Pointer classes ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file declares DIMemoryHandleBase class and its sub-classes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_MEMORY_HANDLE_H
#define TSAR_DI_MEMORY_HANDLE_H

#include <llvm/ADT/DenseMapInfo.h>
#include <llvm/ADT/PointerIntPair.h>

namespace tsar {
class DIMemory;
template<typename From> struct simplify_type;

/// \brief This is the common base class of metadata-level memory handles.
///
/// This is similar to value handles. DIMemoryHandle's are smart pointers to
/// DIMemory's that have special behavior when the memory is deleted or
/// ReplaceAllUsesWith'd. See the specific handles below for details.
class DIMemoryHandleBase {
protected:
  enum Kind {
    Assert,
    Weak
  };
public:
  /// This callback is activated when memory is deleted.
  static void memoryIsDeleted(DIMemory *M);

  /// This callback is activated when memory is RAUWd.
  static void memoryIsRAUWd(DIMemory *Old, DIMemory *New);

  /// Creates handle of a specified type.
  explicit DIMemoryHandleBase(Kind K) : mPrevPair(nullptr, K) {}

  /// Adds new handle of a specified type to handle list for `M`.
  DIMemoryHandleBase(Kind K, DIMemory *M) : mPrevPair(nullptr, K), mMemory(M) {
    if (isValid(M))
      addToUseList();
  }

  /// Removes this handle from a list of memory handles.
  ~DIMemoryHandleBase() {
    if (isValid(mMemory))
      removeFromUseList();
  }

  DIMemory * operator=(DIMemory *RHS) {
    if (mMemory == RHS)
      return RHS;
    if (isValid(mMemory))
      removeFromUseList();
    mMemory = RHS;
    if (isValid(mMemory))
      addToUseList();
    return RHS;
  }

  DIMemory * operator=(const DIMemoryHandleBase &RHS) {
    if (mMemory == RHS.mMemory)
      return RHS.mMemory;
    if (isValid(mMemory))
      removeFromUseList();
    mMemory = RHS.mMemory;
    if (isValid(mMemory))
      addToExistingUseList(RHS.getPrevPtr());
  }

  DIMemory * operator->() const { return mMemory; }
  DIMemory & operator*() const { return *mMemory; }
protected:
  friend class DIMemory;

  static bool isValid(DIMemory *M) {
    return M && M != llvm::DenseMapInfo<DIMemory *>::getEmptyKey() &&
      M != llvm::DenseMapInfo<DIMemory *>::getTombstoneKey();
  }

  DIMemoryHandleBase(const DIMemoryHandleBase &RHS) :
    DIMemoryHandleBase(RHS.mPrevPair.getInt(), RHS) {}

  DIMemoryHandleBase(Kind Kind, const DIMemoryHandleBase &RHS) :
    mPrevPair(nullptr, Kind), mMemory(RHS.mMemory) {
    if (isValid(mMemory))
      addToExistingUseList(RHS.getPrevPtr());
  }

  /// Returns pointer to the underlying memory location.
  DIMemory *getMemoryPtr() const noexcept { return mMemory; }

private:
  /// \brief Returns pointer to a pointer to this handle.
  ///
  /// This pointer is pointed either to the mNext member of a previous handle
  /// of underlying memory or to the pointer in a container of handles map
  /// which points to the first handle of underlying memory.
  DIMemoryHandleBase **getPrevPtr() const { return mPrevPair.getPointer(); }

  /// Returns kind of this handle.
  Kind getKind() const { return mPrevPair.getInt(); }

  /// Sets pointer to a pointer to this handle.
  void setPrevPtr(DIMemoryHandleBase **Ptr) { mPrevPair.setPointer(Ptr); }

  /// \brief Inserts this handle to a list of handles for underlying memory.
  ///
  /// List is the address of either the head of the list or a Next node within
  /// the existing use list.
  void addToExistingUseList(DIMemoryHandleBase **List);

  /// Inserts this handle to a list of handles after a specified node.
  void addToExistingUseListAfter(DIMemoryHandleBase *Node);

  /// Inserts this handle to a list of handles for underlying memory.
  void addToUseList();

  /// Removes this handle from a list of handles for underlying memory.
  void removeFromUseList();

  llvm::PointerIntPair<DIMemoryHandleBase**, 2, Kind> mPrevPair;
  DIMemoryHandleBase *mNext = nullptr;
  DIMemory *mMemory = nullptr;
};

/// Memory handle that is nullable, but tries to track the DIMemory.
///
/// This is a memory handle that tries hard to point to a DIMemory, even across
/// RAUW operations, but will null itself out if the value is destroyed. This
/// is useful for advisory sorts of information, but should not be used as the
/// key of a map (since the map would have to rearrange itself when the pointer
/// changes).
class WeakDIMemoryHandle : public DIMemoryHandleBase {
public:
  WeakDIMemoryHandle() : DIMemoryHandleBase(Weak) {}
  WeakDIMemoryHandle(DIMemory *M) : DIMemoryHandleBase(Weak, M) {}
  WeakDIMemoryHandle(const WeakDIMemoryHandle &RHS) :
    DIMemoryHandleBase(Weak, RHS) {}

  WeakDIMemoryHandle & operator=(const WeakDIMemoryHandle &RHS) = default;

  DIMemory * operator=(DIMemory *RHS) {
    return DIMemoryHandleBase::operator=(RHS);
  }

  operator DIMemory * () const {
    return getMemoryPtr();
  }
};
}

namespace llvm {
// Specialize simplify_type to allow WeakVH to participate in
// dyn_cast, isa, etc.
template<> struct simplify_type<tsar::WeakDIMemoryHandle> {
  using SimpleType = tsar::DIMemory *;
  static SimpleType getSimplifiedValue(tsar::WeakDIMemoryHandle &WMH) {
    return WMH;
  }
};
template<> struct simplify_type<const tsar::WeakDIMemoryHandle> {
  using SimpleType = tsar::DIMemory *;
  static SimpleType getSimplifiedValue(const tsar::WeakDIMemoryHandle &WMH) {
    return WMH;
  }
};
}

namespace tsar {
/// \brief Memory handle that asserts if the DIMemory is deleted.
///
/// This is a Memory Handle that points to a memory and asserts out if the
/// memory is destroyed while the handle is still live. This is very useful for
/// catching dangling pointer bugs and other things which can be non-obvious.
/// One particularly useful place to use this is as the Key of a map.  Dangling
/// pointer bugs often lead to really subtle bugs that only occur if another
/// object happens to get allocated to the same address as the old one. Using
/// an AssertingDIMemoryHandle ensures that an assert is triggered as soon as
/// the bad delete occurs.
///
/// Note that an AssertingDIMemoryHandle handle does *not* follow values across
/// RAUW operations.  This means that RAUW's need to explicitly update the
/// AssertingDIMemoryHandle's as it moves. This is required because in
/// non-assert mode this class turns into a trivial wrapper around a pointer.
template<class MemoryTy>
class AssertingDIMemoryHandle
#ifndef NDEBUG
  : public DIMemoryHandleBase
#endif
{
public:
#ifndef NDEBUG
  AssertingDIMemoryHandle() : DIMemoryHandleBase(Assert) {}
  AssertingDIMemoryHandle(MemoryTy *P) :
    DIMemoryHandleBase(Assert, getAsMemory(P)) {}
  AssertingDIMemoryHandle(const AssertingDIMemoryHandle &RHS) :
    DIMemoryHandleBase(Assert, RHS) {}
#else
  AssertingDIMemoryHandle() : mPtr(nullptr) {}
  AssertingDIMemoryHandle(MemoryTy *P) : mPtr(getAsValue(P)) {}
#endif

  operator MemoryTy * () const { return getMemoryPtr(); }

  MemoryTy * operator=(MemoryTy *RHS) {
    setMemoryPtr(RHS);
    return getMemoryPtr();
  }

  MemoryTy * operator=(const AssertingDIMemoryHandle<MemoryTy> &RHS) {
    setMemoryPtr(RHS.getMemoryPtr());
    return getMemoryPtr();
  }

  MemoryTy * operator->() const { return getMemoryPtr(); }
  MemoryTy & operator*() const { return *getMemoryPtr(); }

private:
  friend struct llvm::DenseMapInfo<AssertingDIMemoryHandle<MemoryTy>>;
#ifndef NDEBUG
  DIMemory * getRawMemoryPtr() const {
    return DIMemoryHandleBase::getMemoryPtr();
  }
  void setRawMemoryPtr(DIMemory *P) { DIMemoryHandleBase::operator=(P); }
#else
  DIMemory *mPtr;
  DIMemory *getRawMemoryPtr() const { return mPtr; }
  void setRawValPtr(DIMemory *P) { mPtr = P; }
#endif
  // Convert a ValueTy*, which may be const, to the raw Value*.
  static DIMemory * getAsMemory(DIMemory *M) { return M; }
  static DIMemory * getAsMemory(const DIMemory *M) {
    return const_cast<DIMemory*>(M);
  }

  MemoryTy * getMemoryPtr() const {
    return static_cast<MemoryTy *>(getRawMemoryPtr());
  }
  void setMemoryPtr(MemoryTy *M) { setRawMemoryPtr(getAsMemory(M)); }
};
}

namespace llvm {
template<class T>
struct DenseMapInfo<tsar::AssertingDIMemoryHandle<T>> {
  static inline tsar::AssertingDIMemoryHandle<T> getEmptyKey() {
    tsar::AssertingDIMemoryHandle<T> Res;
    Res.setRawMemoryPtr(DenseMapInfo<tsar::DIMemory *>::getEmptyKey());
    return Res;
  }
  static inline tsar::AssertingDIMemoryHandle<T> getTombstoneKey() {
    tsar::AssertingDIMemoryHandle<T> Res;
    Res.setRawMemoryPtr(DenseMapInfo<tsar::DIMemory *>::getTombstoneKey());
    return Res;
  }
  static unsigned getHashValue(const tsar::AssertingDIMemoryHandle<T> &M) {
    return DenseMapInfo<tsar::DIMemory *>::getHashValue(M.getRawMemoryPtr());
  }
  static bool isEqual(const tsar::AssertingDIMemoryHandle<T> &LHS,
    const tsar::AssertingDIMemoryHandle<T> &RHS) {
    return DenseMapInfo<tsar::DIMemory *>::isEqual(
      LHS.getRawMemoryPtr(), RHS.getRawMemoryPtr());
  }
};

template <typename T>
struct isPodLike<tsar::AssertingDIMemoryHandle<T> > {
#ifdef NDEBUG
  static const bool value = true;
#else
  static const bool value = false;
#endif
};
}
#endif//TSAR_DI_MEMORY_HANDLE_H
