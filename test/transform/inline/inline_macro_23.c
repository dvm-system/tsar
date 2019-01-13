void f() {
  #define M 10
}

void f1() {
  #pragma spf transform inline
  f();
}
//CHECK: inline_macro_23.c:1:6: warning: disable inline expansion
//CHECK: void f() {
//CHECK:      ^
//CHECK: inline_macro_23.c:2:11: note: macro prevent inlining
//CHECK:   #define M 10
//CHECK:           ^
//CHECK: 1 warning generated.
