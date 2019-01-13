
void f();

void f1() {
  #pragma spf transform inline
  f();
}

struct A {int X;};

struct S {
enum { SIZE = 10 };
};

void f() { int N[SIZE]; }
//CHECK: inline_forward_3.c:6:3: warning: disable inline expansion
//CHECK:   f();
//CHECK:   ^
//CHECK: inline_forward_3.c:12:8: note: unresolvable external dependence prevents inlining
//CHECK: enum { SIZE = 10 };
//CHECK:        ^
//CHECK: 1 warning generated.
