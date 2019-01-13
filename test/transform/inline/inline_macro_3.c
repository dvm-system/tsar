void f() { int X;}

#define MACRO(f_) f_();

void f1() {
#pragma spf transform inline
  MACRO(f)
}
//CHECK: inline_macro_3.c:7:9: warning: disable inline expansion
//CHECK:   MACRO(f)
//CHECK:         ^
//CHECK: inline_macro_3.c:7:9: note: macro prevent inlining
//CHECK: 1 warning generated.
