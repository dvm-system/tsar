int foo() {
  int X = 45;
  return X;
}

int foo_1() {
  int X = 9;
  return X;
}

void bar() {
  int X = 0;
  /* foo_1() is inlined below */
  int R3;
#pragma spf assert nomacro
  {
    int X = 9;
    R3 = X;
  }
  /* foo() is inlined below */
  int R2;
#pragma spf assert nomacro
  {
    int X = 45;
    R2 = X;
  }
  X = R2 + R3;
}

int main() {
  int X = 0;
  {
    /* foo_1() is inlined below */
    int R1;
#pragma spf assert nomacro
    {
      int X = 9;
      R1 = X;
    }
    /* foo() is inlined below */
    int R0;
#pragma spf assert nomacro
    {
      int X = 45;
      R0 = X;
    }
    X = R0 + R1;
    //  bar();
  }
  return 0;
}
