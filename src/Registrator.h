#ifndef REGISTRATOR_H
#define REGISTRATOR_H

class Registrator {
private:
  unsigned mVarCounter;
  unsigned mDbgStrCounter;
public:
  Registrator(): mVarCounter(0), mDbgStrCounter(0) {};
  unsigned regVar() { return mVarCounter++; }
  unsigned regArr() { return mVarCounter++; }
  unsigned regLoc() { return mVarCounter++; }
  unsigned regDbgStr() { return mDbgStrCounter++; }
};

#endif //REGISTRATOR_H
