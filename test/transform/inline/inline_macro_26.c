void f() {}

void g() {
# pragma spf transform inline
  f(
#define M 1
  );
}
//CHECK: inline_macro_26.c:5:3: warning: disable inline expansion
//CHECK:   f(
//CHECK:   ^
//CHECK: inline_macro_26.c:6:1: note: macro prevent inlining
//CHECK: #define M 1
//CHECK: ^
//CHECK: 1 warning generated.
