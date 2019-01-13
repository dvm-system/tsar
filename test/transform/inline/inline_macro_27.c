int f() { return 4; }

#define MACRO() x + 

void f1() {
  int x;
#pragma spf transform inline
  MACRO() f();
}
//CHECK: inline_macro_27.c:8:11: warning: disable inline expansion
//CHECK:   MACRO() f();
//CHECK:           ^
//CHECK: inline_macro_27.c:8:3: note: macro prevent inlining
//CHECK:   MACRO() f();
//CHECK:   ^
//CHECK: inline_macro_27.c:3:9: note: expanded from here
//CHECK: #define MACRO() x + 
//CHECK:         ^
//CHECK: 1 warning generated.
