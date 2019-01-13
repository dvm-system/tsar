
void f();

void f1() { f(); }

struct A {
  int X;
};

struct S {
  enum { SIZE = 10 };
};

void f() { int N[SIZE]; }
