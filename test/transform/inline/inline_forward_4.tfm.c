int f1();

int f() { return f1(); }

struct S {
  int X;
};

int f1() {
  struct S S1;
  S1.X = 5;
  return S1.X;
}
