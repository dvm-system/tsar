//This test contains macro in a location which is omitted in the AST.

#define C :

void f() {
L C return;
}

void f1() {
  #pragma spf transform inline
  f();
}
//CHECK: inline_macro_22.c:5:6: warning: disable inline expansion
//CHECK: void f() {
//CHECK:      ^
//CHECK: inline_macro_22.c:6:3: note: macro prevent inlining
//CHECK: L C return;
//CHECK:   ^
//CHECK: inline_macro_22.c:3:9: note: expanded from here
//CHECK: #define C :
//CHECK:         ^
//CHECK: 1 warning generated.
