void f(void (*h)(int), int I) { h(I); }

void g(int I) {

  /* f(g, I) is inlined below */
#pragma spf assert nomacro
  {
    void (*h0)(int) = g;
    int I0 = I;
    h0(I0);
  }
}
