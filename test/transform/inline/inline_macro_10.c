int f1() { return 0;}

#define MACRO + x

void f2() {
  int x;
#pragma spf transform inline
  f1() MACRO;
}
//CHECK: inline_macro_10.c:8:3: warning: disable inline expansion
//CHECK:   f1() MACRO;
//CHECK:   ^
//CHECK: inline_macro_10.c:8:8: note: macro prevent inlining
//CHECK:   f1() MACRO;
//CHECK:        ^
//CHECK: inline_macro_10.c:3:9: note: expanded from here
//CHECK: #define MACRO + x
//CHECK:         ^
//CHECK: 1 warning generated.
