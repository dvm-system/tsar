int f() { return 0;}

#define MACRO x+x;

void f2() {
  int x;
#pragma spf transform inline
  MACRO f();
}
//CHECK: inline_macro_6.c:7:23: warning: unexpected directive ignored
//CHECK: #pragma spf transform inline
//CHECK:                       ^
//CHECK: inline_macro_6.c:8:3: note: no call suitable for inline is found
//CHECK:   MACRO f();
//CHECK:   ^
//CHECK: inline_macro_6.c:3:15: note: expanded from macro 'MACRO'
//CHECK: #define MACRO x+x;
//CHECK:               ^
//CHECK: 1 warning generated.
