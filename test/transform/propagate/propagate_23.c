#include <math.h>

int bar();

int foo(int Z) {
#pragma spf transform propagate
  int X;
  int (*f)() = bar;
  X = abs(Z);
  return X;
}
//CHECK: 
