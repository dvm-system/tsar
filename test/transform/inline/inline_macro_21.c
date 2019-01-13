#define M

void f() {
  M;
}

void f1() {
  #pragma spf transform inline
  f();
}
//CHECK: inline_macro_21.c:3:6: warning: disable inline expansion
//CHECK: void f() {
//CHECK:      ^
//CHECK: inline_macro_21.c:4:3: note: macro prevent inlining
//CHECK:   M;
//CHECK:   ^
//CHECK: inline_macro_21.c:1:9: note: expanded from here
//CHECK: #define M
//CHECK:         ^
//CHECK: 1 warning generated.
