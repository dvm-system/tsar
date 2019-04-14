struct A {
  int X;
};

int f(int (*g)(struct A)) {
  struct A A1;
  A1.X = 5;
  return g(A1);
}

int g(struct A);

void f1() {

  /* f(g) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int (*g0)(struct A) = g;
    struct A A1;
    A1.X = 5;
    R0 = g0(A1);
  }
  R0;
}
