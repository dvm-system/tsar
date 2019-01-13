#define R )

int f() {
  int X;
  return (int R X;
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_17.c:3:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_17.c:5:15: note: macro prevent inlining
//CHECK:   return (int R X;
//CHECK:               ^
//CHECK: inline_macro_17.c:1:11: note: expanded from macro 'R'
//CHECK: #define R )
//CHECK:           ^
//CHECK: 1 warning generated.
