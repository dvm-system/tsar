#ifndef REGISTRATOR_H
#define REGISTRATOR_H

#include "tsar_utility.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Type.h>
#include <map>
#include <vector>

class Registrator {
public:
  /// Identifier of an item.
  using IdTy = uint64_t;

  /// This establishes correspondence between a type and its identifier.
  using Types = llvm::DenseMap<const llvm::Type *, IdTy,
    llvm::DenseMapInfo<const llvm::Type *>,
    tsar::TaggedDenseMapPair<
      bcl::tagged<const llvm::Type *, llvm::Type>,
      bcl::tagged<IdTy, IdTy>>>;

private:
  unsigned mDbgStrCounter;
  //all of the registered variables with their indexes
  std::map<const llvm::Value*, unsigned> mRegVars;
  //all of the registered functions
  std::map<const llvm::Function*, unsigned> mRegFuncs;
public:
  Registrator():  mDbgStrCounter(0) {};

  /// Returns number of IDs that have been registered.
  IdTy numberOfIDs() const noexcept { return mIdNum; }

  //registrate variable with given debug index. if this variable was already
  //registrated returns its index in debug pool  
  unsigned regVar(const llvm::Value* V, unsigned Idx) {
    auto search = mRegVars.find(V);
    if(search != mRegVars.end())
      return search->second;
    mRegVars[V] = Idx;
    return Idx;
  }
  //registrate array with given debug index. if this array was already
  //registrated returns its index in debug pool  
  unsigned regArr(const llvm::Value* V, unsigned Idx) {
    auto search = mRegVars.find(V);
    if(search != mRegVars.end())
      return search->second;
    mRegVars[V] = Idx;
    return Idx;
  }
  //registrate function with given debug index. if this function was already
  //registrated returns its index in debug pool  
  unsigned regFunc(const llvm::Function* F, unsigned Idx) {
    auto search = mRegFuncs.find(F);
    if(search != mRegFuncs.end())
      return search->second;
    mRegFuncs[F] = Idx;
    return Idx;
  }
  unsigned regDbgStr() { return mDbgStrCounter++; }
  unsigned getDbgStrCounter() { return mDbgStrCounter; }

  /// Registers a new type if it has not been registered yet and
  /// returns its ID.
  unsigned regType(const llvm::Type *T) {
    assert(T && "Type must not be null!");
    auto Pair = mRegTypes.try_emplace(T, mIdNum);
    if (Pair.second)
      ++mIdNum;
    return Pair.first->get<IdTy>();
  }

  /// Returns all types that have been registered.
  const Types & getTypes() const noexcept {
    return mRegTypes;
  }

  //returns index in debug pool for given variable
  unsigned getVarDbgIndex(const llvm::Value* V)  {
    auto search = mRegVars.find(V);
    if(search != mRegVars.end())
      return search->second;
    llvm_unreachable((V->getName().str() + " was not declared").c_str());
  }
  //returns index in debug pool for given function
  unsigned getFuncDbgIndex(const llvm::Function* F)  {
    auto search = mRegFuncs.find(F);
    if(search != mRegFuncs.end())
      return search->second;
    llvm_unreachable((F->getName().str() + " was not declared").c_str());
  }

private:
  /// Number of used IDs.
  IdTy mIdNum = 0;
  Types mRegTypes;
};

#endif //REGISTRATOR_H
