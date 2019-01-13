void f(int *X) { *X = 10; }

void g() {
  int Y;
  _Pragma("spf transform propagate")
  /* f(&Y) is inlined below */
#pragma spf assert nomacro
  {
    int *X0 = &Y;
    *X0 = 10;
  }
}
