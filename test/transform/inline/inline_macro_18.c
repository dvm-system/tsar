#define Q ?

int f() {
 return 1 Q 0 : 0;
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_18.c:3:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_18.c:4:11: note: macro prevent inlining
//CHECK:  return 1 Q 0 : 0;
//CHECK:           ^
//CHECK: inline_macro_18.c:1:11: note: expanded from macro 'Q'
//CHECK: #define Q ?
//CHECK:           ^
//CHECK: 1 warning generated.
