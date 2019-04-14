int f2() { return 2; }
int f3() {

  /* f2() is inlined below */
  int R2;
#pragma spf assert nomacro
  { R2 = 2; }
  return R2 + 3;
}

int f1() {

  /* f3() is inlined below */
  int R1;
#pragma spf assert nomacro
  {
    /* f2() is inlined below */
    int R0;
#pragma spf assert nomacro
    { R0 = 2; }
    R1 = R0 + 3;
  }
  return R1 + 1;
}
