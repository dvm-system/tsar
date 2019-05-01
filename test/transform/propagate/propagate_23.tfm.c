#include <math.h>

int bar();

int foo(int Z) {
#pragma spf assert nomacro
  {

    int X;
    int (*f)() = bar;
    X = abs(Z);
    return (abs(Z));
  }
}
