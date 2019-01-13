#define MACRO 5, 5

int f() { return 0; }

void f2(int y, int z) {
}

void f3() {
#pragma spf transform inline
  f2(MACRO);
}
//CHECK: inline_macro_8.c:10:3: warning: disable inline expansion
//CHECK:   f2(MACRO);
//CHECK:   ^
//CHECK: inline_macro_8.c:10:6: note: macro prevent inlining
//CHECK:   f2(MACRO);
//CHECK:      ^
//CHECK: inline_macro_8.c:1:15: note: expanded from macro 'MACRO'
//CHECK: #define MACRO 5, 5
//CHECK:               ^
//CHECK: 1 warning generated.
