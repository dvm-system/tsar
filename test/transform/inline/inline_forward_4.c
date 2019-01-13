int f1();

int f() {
#pragma spf transform inline
  return f1();
}

struct S { int X; };

int f1() {
  struct S S1;
  S1.X = 5;
  return S1.X;
}
//CHECK: inline_forward_4.c:5:10: warning: disable inline expansion
//CHECK:   return f1();
//CHECK:          ^
//CHECK: inline_forward_4.c:8:8: note: unresolvable external dependence prevents inlining
//CHECK: struct S { int X; };
//CHECK:        ^
//CHECK: 1 warning generated.
