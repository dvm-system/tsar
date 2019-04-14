void f(int X) {}

void g(int X) {

  /* f(X) is inlined below */
#pragma spf assert nomacro
  { int X4 = X; }
}

void h() {
  int X0, X1;

  /* g(X1) is inlined below */
#pragma spf assert nomacro
  {
    int X2 = X1;
    /* f(X) is inlined below */
#pragma spf assert nomacro
    { int X3 = X2; }
  }
}
