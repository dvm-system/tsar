class A {
  void f() { X = 5; }
  int X;
};

A f() {
  A A1;
  return A();
}

void g() {

  /* f() is inlined below */
  class A R0;
#pragma spf assert nomacro
  {
    A A1;
    R0 = A();
  }
  R0;
}
