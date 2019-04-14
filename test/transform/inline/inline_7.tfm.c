int f(int I) {
  if (I)
    return 5;
  return 10;
}

void g() {

  /* f(1) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int I0 = 1;
    if (I0) {
      R0 = 5;
      goto L0;
    }
    R0 = 10;
  }
L0:;
  R0;
}
