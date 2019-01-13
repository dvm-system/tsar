#define M "b"

char * f() {
  return "a" M
  "c";
}

void f1() {
  #pragma spf transform inline
  f();
}
//CHECK: inline_macro_14.c:3:8: warning: disable inline expansion
//CHECK: char * f() {
//CHECK:        ^
//CHECK: inline_macro_14.c:4:14: note: macro prevent inlining
//CHECK:   return "a" M
//CHECK:              ^
//CHECK: inline_macro_14.c:1:11: note: expanded from macro 'M'
//CHECK: #define M "b"
//CHECK:           ^
//CHECK: 1 warning generated.
