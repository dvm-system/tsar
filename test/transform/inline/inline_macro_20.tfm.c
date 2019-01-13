#include "inline_macro_20.h"

#define A X
#define B A + A
#define C B + B

#define D(_d) _d

#define E(_e) D(_e)

#define Q(_q) _q

int f() { return Q(E(C)); }

void f1() { f(); }
