int M() { return 78; }

int M1() { return 79; }

int F(int X) {
  /* M() is inlined below */
  int R5;
#pragma spf assert nomacro
  { R5 = 78; }
  /* M1() is inlined below */
  int R6;
#pragma spf assert nomacro
  { R6 = 79; }
  return X + R5 + R6;
}

int main() {
  int x = 0;

  /* M() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 78; }
  /* M1() is inlined below */
  int R1;
#pragma spf assert nomacro
  { R1 = 79; }
  /* F(M() + M1()) is inlined below */
  int R4;
#pragma spf assert nomacro
  {
    int X0 = R0 + R1;
    /* M() is inlined below */
    int R2;
#pragma spf assert nomacro
    { R2 = 78; }
    /* M1() is inlined below */
    int R3;
#pragma spf assert nomacro
    { R3 = 79; }
    R4 = X0 + R2 + R3;
  }
  x += R4;
  return 0;
}
