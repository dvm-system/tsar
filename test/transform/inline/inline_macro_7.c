#define MACRO(f_) f_() +

int f1() { return 0; }

int f2() {
#pragma spf transform inline
  return MACRO(f1) 4;
}
//CHECK: inline_macro_7.c:7:16: warning: disable inline expansion
//CHECK:   return MACRO(f1) 4;
//CHECK:                ^
//CHECK: inline_macro_7.c:7:16: note: macro prevent inlining
//CHECK: 1 warning generated.
