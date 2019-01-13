#define CALL f()

void f() {}

void f1() {
#pragma spf transform inline
  CALL;
}
//CHECK: inline_macro_1.c:7:3: warning: disable inline expansion
//CHECK:   CALL;
//CHECK:   ^
//CHECK: inline_macro_1.c:1:14: note: expanded from macro 'CALL'
//CHECK: #define CALL f()
//CHECK:              ^
//CHECK: inline_macro_1.c:7:3: note: macro prevent inlining
//CHECK: inline_macro_1.c:1:14: note: expanded from macro 'CALL'
//CHECK: #define CALL f()
//CHECK:              ^
//CHECK: 1 warning generated.
