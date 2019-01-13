#define C const

void f() {
  int C * A;
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_16.c:3:6: warning: disable inline expansion
//CHECK: void f() {
//CHECK:      ^
//CHECK: inline_macro_16.c:4:7: note: macro prevent inlining
//CHECK:   int C * A;
//CHECK:       ^
//CHECK: inline_macro_16.c:1:9: note: expanded from here
//CHECK: #define C const
//CHECK:         ^
//CHECK: 1 warning generated.
