#ifndef REGISTRATOR_H
#define REGISTRATOR_H

#include <llvm/IR/Type.h>
#include <map>
#include <vector>

class Registrator {
private:
  unsigned mDbgStrCounter;
  unsigned mTypesCounter;
  //all of the registered types with their indexes
  std::map<const llvm::Type*, unsigned> mRegTypes;
  //all of the registered variables with their indexes
  std::map<const llvm::Value*, unsigned> mRegVars;
public:
  Registrator():  mDbgStrCounter(0), mTypesCounter(0) {};
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
  unsigned regDbgStr() { return mDbgStrCounter++; }
  unsigned getDbgStrCounter() { return mDbgStrCounter; }
  //registrate new type if it was not registrated yet. returns type index. 
  unsigned regType(const llvm::Type* T) {
    auto search = mRegTypes.find(T);
    if(search != mRegTypes.end())
      return search->second;
    unsigned Idx = mTypesCounter++;
    mRegTypes[T] = Idx;
    return Idx;
  }
  //returns index in debug pool for given variable
  unsigned getVarDbgIndex(const llvm::Value* V)  {
    auto search = mRegVars.find(V);
    if(search != mRegVars.end())
      return search->second;
    llvm_unreachable((V->getName().str() + " was not declared").c_str());
  }
  const std::map<const llvm::Type*, unsigned>& getAllRegistratedTypes()
  { return mRegTypes; }
};

#endif //REGISTRATOR_H
