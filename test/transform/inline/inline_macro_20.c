#include "inline_macro_20.h"

#define A X
#define B A + A
#define C B + B

#define D(_d) _d


#define E(_e) D(_e)

#define Q(_q) _q

int f() {
return Q(E(C));
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_20.c:14:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_20.c:15:12: note: macro prevent inlining
//CHECK: return Q(E(C));
//CHECK:            ^
//CHECK: inline_macro_20.c:5:15: note: expanded from macro 'C'
//CHECK: #define C B + B
//CHECK:               ^
//CHECK: inline_macro_20.c:4:15: note: expanded from macro 'B'
//CHECK: #define B A + A
//CHECK:               ^
//CHECK: inline_macro_20.c:3:11: note: expanded from macro 'A'
//CHECK: #define A X
//CHECK:           ^
//CHECK: inline_macro_20.h:1:16: note: expanded from macro 'X'
//CHECK:    #  define X 10
//CHECK:                ^
//CHECK: 1 warning generated.
