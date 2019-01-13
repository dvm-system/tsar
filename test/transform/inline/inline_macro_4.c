void f() { int X;}

#define MACRO(f_) f_(); X = 5;

void f1() {
  int X;
#pragma spf transform inline
  MACRO(f)
}
//CHECK: inline_macro_4.c:8:9: warning: disable inline expansion
//CHECK:   MACRO(f)
//CHECK:         ^
//CHECK: inline_macro_4.c:8:9: note: macro prevent inlining
//CHECK: 1 warning generated.
