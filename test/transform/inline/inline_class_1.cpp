struct    A {
  void f() { X = 5; }
  int X;
};

struct    A f() {
  A A1;
  return A();
}

void g() {
  #pragma spf transform inline
  f();
}
//CHECK: warning: inline expansion in C++ sources is not fully supported
//CHECK: 1 warning generated.
