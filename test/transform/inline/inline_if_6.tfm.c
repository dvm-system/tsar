int f() { return 1; }

int g(int X) {

  /* f() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 1; }
  if (R0 && X)
    return X;
  return 0;
}
