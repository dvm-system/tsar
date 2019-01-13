#define MACRO f(), 5

int f() { return 0; }

void f2(int y, int z) {
}

void f3() {
#pragma spf transform inline
  f2(MACRO);
}
//CHECK: inline_macro_9.c:10:6: warning: disable inline expansion
//CHECK:   f2(MACRO);
//CHECK:      ^
//CHECK: inline_macro_9.c:1:15: note: expanded from macro 'MACRO'
//CHECK: #define MACRO f(), 5
//CHECK:               ^
//CHECK: inline_macro_9.c:10:6: note: macro prevent inlining
//CHECK: inline_macro_9.c:1:15: note: expanded from macro 'MACRO'
//CHECK: #define MACRO f(), 5
//CHECK:               ^
//CHECK: inline_macro_9.c:10:3: warning: disable inline expansion
//CHECK:   f2(MACRO);
//CHECK:   ^
//CHECK: inline_macro_9.c:10:6: note: macro prevent inlining
//CHECK:   f2(MACRO);
//CHECK:      ^
//CHECK: inline_macro_9.c:1:15: note: expanded from macro 'MACRO'
//CHECK: #define MACRO f(), 5
//CHECK:               ^
//CHECK: 2 warnings generated.
