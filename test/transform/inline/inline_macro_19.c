#define C :

int f() {
 return 1 ? 0 C 0;
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_19.c:3:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_19.c:4:15: note: macro prevent inlining
//CHECK:  return 1 ? 0 C 0;
//CHECK:               ^
//CHECK: inline_macro_19.c:1:11: note: expanded from macro 'C'
//CHECK: #define C :
//CHECK:           ^
//CHECK: 1 warning generated.
