#ifndef REGISTRATOR_H
#define REGISTRATOR_H

class Registrator {
private:
  unsigned mCounter;
public:
  Registrator(): mCounter(0) {};
  unsigned regVar() { return mCounter++; }
  unsigned regArr() { return mCounter++; }
  unsigned regLoc() { return mCounter++; }
};

#endif //REGISTRATOR_H
