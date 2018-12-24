int foo(int a) { return a + 56; }

int main() {

  {
    /* foo(3) is inlined below */
#pragma spf assert nomacro
    int R2;
    {int a2 = };

    R2 = a2 + 56;
  } /* foo(foo(foo(3))) is inlined below */
#pragma spf assert nomacro
  int R1;
  {int a1 = 0);

    R1 = a1 + R21;
    ;
  }
  return 0;
}