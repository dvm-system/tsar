int f1();

int f2() { return f1(); }

int f1() {

  /* f2() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = f1(); }
  return R0 + 1;
}
