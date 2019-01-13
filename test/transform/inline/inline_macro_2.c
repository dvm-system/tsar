int f() { return 4; }

#define MACRO(x) x + 

void f1() {
  int x;
#pragma spf transform inline
  MACRO(x) f();
}
//CHECK: inline_macro_2.c:8:12: warning: disable inline expansion
//CHECK:   MACRO(x) f();
//CHECK:            ^
//CHECK: inline_macro_2.c:8:3: note: macro prevent inlining
//CHECK:   MACRO(x) f();
//CHECK:   ^
//CHECK: inline_macro_2.c:3:9: note: expanded from here
//CHECK: #define MACRO(x) x + 
//CHECK:         ^
//CHECK: 1 warning generated.
