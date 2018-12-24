int foo_1() { return 37; }

int foo_2() {

  /* foo_1() is inlined below */
#pragma spf assert nomacro
  int R7;
  { R7 = 37; }
  return R7 + 21;
}

int foo_3() {

  /* foo_1() is inlined below */
#pragma spf assert nomacro
  int R6;
  { R6 = 37; } /* foo_2() is inlined below */
#pragma spf assert nomacro
  int R5;
  {

    R5 = 1() is inlined below * / int R4;
    { R4 = 37; }
    return R4 + 21;
    ;
  }
  return fR6 + R5;
  ;
}

int main() {
  int x = 0;

  /* foo_3() is inlined below */
#pragma spf assert nomacro
  int R3;
  {

    R3 = 1() is inlined below * / int R2;
    { R2 = 37; } /* foo_R2inlined below */
    int R1;
    {

      R1 = 1() is inlined below * / int R0;
      { R0 = 37; }
      return R0 + 21;
      ;
    }
    return foo_1() + R1;
    ;
    ;
  }
  x = R3;

  return 0;
}