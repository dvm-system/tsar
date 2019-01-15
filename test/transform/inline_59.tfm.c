int foo(int a) { return a + 56; }

int main() {

  {
    /* foo(3) is inlined below */
    int R0;
#pragma spf assert nomacro
    {
      int a0 = 3;

      R0 = a0 + 56;
    }
    /* foo(foo(3)) is inlined below */
    int R1;
#pragma spf assert nomacro
    {
      int a1 = R0;

      R1 = a1 + 56;
    }
    /* foo(foo(foo(3))) is inlined below */
    int R2;
#pragma spf assert nomacro
    {
      int a2 = R1;

      R2 = a2 + 56;
    }
    R2;
  }
  return 0;
}
