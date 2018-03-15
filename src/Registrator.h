#ifndef REGISTRATOR_H
#define REGISTRATOR_H

#include <llvm/IR/Type.h>
#include <map>
#include <vector>

class Registrator {
private:
  struct Variable {
    Variable(const std::string& S, unsigned L, bool G)
      :Name(S), Line(L), Global(G) {}
    std::string Name;
    unsigned Line;
    unsigned DbgIndex;
    bool Global;
  };
  unsigned mDbgStrCounter; 
  unsigned mTypesCounter;
  //all of the registered types with their indexes
  std::map<const llvm::Type*, unsigned> mRegTypes;
  //all of the registered variables
  std::vector<Variable> mRegVars;
public:
  Registrator():  mDbgStrCounter(0), mTypesCounter(0) {};
  //registrate variable. set its debug index to the current value of 
  //DbgStrCounter. should be called right after regDbgStr if the registered
  //string is assotiated with new variable.
  unsigned regVar(const std::string& Name, unsigned Line, bool Global = false) { 
    Variable V(Name, Line, Global);
    V.DbgIndex = mDbgStrCounter;
    mRegVars.push_back(V);
    return mDbgStrCounter;
  }
  //registrate array. set its debug index to the current value of 
  //DbgStrCounter. should be called right after regDbgStr if the registered
  //string is assotiated with new array.
  unsigned regArr(const std::string& Name, unsigned Line, bool Global = false) {
    Variable V(Name, Line, Global);
    V.DbgIndex = mDbgStrCounter;
    mRegVars.push_back(V);
    return mDbgStrCounter;
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
  //looking for registered variable with name S, which was declared between 
  //lines L1 and L2
  unsigned getVarDbgIndex(const std::string& S, unsigned L1, unsigned L2)  {
    for(auto& I: mRegVars) {
      //looking for local variable
      if((I.Name.compare(S) == 0) && (I.Line > L1) && (I.Line < L2) 
        && !I.Global) {
	return I.DbgIndex;
      }
    }
    //if local variable was not found, search in globals
    for(auto& I: mRegVars) {
      if((I.Name.compare(S) == 0) && I.Global) {
	return I.DbgIndex;
      }
    }
    //nothing found
    llvm_unreachable((S + " was not declared").c_str());
  }
};

#endif //REGISTRATOR_H
