#define M +
int f() {
  return 5 M 5;
}

void f1() {
  #pragma spf transform inline
  {
    f();
  }
}
//CHECK: inline_macro_12.c:2:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_12.c:3:12: note: macro prevent inlining
//CHECK:   return 5 M 5;
//CHECK:            ^
//CHECK: inline_macro_12.c:1:11: note: expanded from macro 'M'
//CHECK: #define M +
//CHECK:           ^
//CHECK: 1 warning generated.
