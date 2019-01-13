int f() {
  return __LINE__;
}

int f1() {
  #pragma spf transform inline
  return f();
}
//CHECK: inline_macro_13.c:1:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_13.c:2:10: note: macro prevent inlining
//CHECK:   return __LINE__;
//CHECK:          ^
//CHECK: <scratch space>:2:1: note: expanded from here
//CHECK: 2
//CHECK: ^
//CHECK: 1 warning generated.
