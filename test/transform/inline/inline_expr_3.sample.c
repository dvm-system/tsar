int M() { return 78; }

int M1() { return 79; }

int F(int X) {
  /* M() is inlined below */
  int R4;
#pragma spf assert nomacro
  { R4 = 78; }
  /* M1() is inlined below */
  int R5;
#pragma spf assert nomacro
  { R5 = 79; }
  return X + R4 + R5;
}

int main() {
  int x = 0;

  /* M() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 78; }
  /* F(M()) is inlined below */
  int R3;
#pragma spf assert nomacro
  {
    int X0 = R0;
    /* M() is inlined below */
    int R1;
#pragma spf assert nomacro
    { R1 = 78; }
    /* M1() is inlined below */
    int R2;
#pragma spf assert nomacro
    { R2 = 79; }
    R3 = X0 + R1 + R2;
  }
  x += R3;
  return 0;
}
