#define M 5

int f() {
  return M;
}

#undef M
#define M 6

int f1() {
#pragma spf transform inline
  return f() + M;
}
//CHECK: inline_macro_11.c:12:10: warning: disable inline expansion
//CHECK:   return f() + M;
//CHECK:          ^
//CHECK: inline_macro_11.c:12:16: note: macro prevent inlining
//CHECK:   return f() + M;
//CHECK:                ^
//CHECK: inline_macro_11.c:1:9: note: expanded from here
//CHECK: #define M 5
//CHECK:         ^
//CHECK: 1 warning generated.
