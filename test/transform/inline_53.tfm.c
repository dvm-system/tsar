int foo_1() { return 37; }

int foo_2() {

  /* foo_1() is inlined below */
  int R7;
#pragma spf assert nomacro
  { R7 = 37; }
  return R7 + 21;
}

int foo_3() {

  /* foo_1() is inlined below */
  int R4;
#pragma spf assert nomacro
  { R4 = 37; }
  /* foo_2() is inlined below */
  int R6;
#pragma spf assert nomacro
  {

    /* foo_1() is inlined below */
    int R5;
#pragma spf assert nomacro
    { R5 = 37; }
    R6 = R5 + 21;
  }
  return R4 + R6;
}

int main() {
  int x = 0;

  /* foo_3() is inlined below */
  int R3;
#pragma spf assert nomacro
  {

    /* foo_1() is inlined below */
    int R0;
#pragma spf assert nomacro
    { R0 = 37; }
    /* foo_2() is inlined below */
    int R2;
#pragma spf assert nomacro
    {

      /* foo_1() is inlined below */
      int R1;
#pragma spf assert nomacro
      { R1 = 37; }
      R2 = R1 + 21;
    }
    R3 = R0 + R2;
  }
  x = R3;

  return 0;
}
