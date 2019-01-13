#define ARRAY A

void f() {
  int ARRAY[10];
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_15.c:3:6: warning: disable inline expansion
//CHECK: void f() {
//CHECK:      ^
//CHECK: inline_macro_15.c:4:7: note: macro prevent inlining
//CHECK:   int ARRAY[10];
//CHECK:       ^
//CHECK: inline_macro_15.c:1:15: note: expanded from macro 'ARRAY'
//CHECK: #define ARRAY A
//CHECK:               ^
//CHECK: 1 warning generated.
