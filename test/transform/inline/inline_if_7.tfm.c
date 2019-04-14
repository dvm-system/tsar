int foo() { return 221; }

int bar(int Val) {

  {
    if (Val)
    /* foo() is inlined below */
    {
      int R2;
#pragma spf assert nomacro
      { R2 = 221; }
      return R2 + Val;
    }
  }
  return Val;
}

int main() {

  /* bar(13) is inlined below */
  int R1;
#pragma spf assert nomacro
  {
    int Val0 = 13;
    {
      if (Val0)
      /* foo() is inlined below */
      {
        int R0;
#pragma spf assert nomacro
        { R0 = 221; }
        {
          R1 = R0 + Val0;
          goto L1;
        }
      }
    }
    R1 = Val0;
  }
L1:;
  return R1;
}
