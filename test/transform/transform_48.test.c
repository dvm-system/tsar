int foo(int a) { return 89 + a; }

int main() {
  int n = 7;

  {
    switch (n) {

    case 1:

      /* foo(0) is inlined below */
#pragma spf assert nomacro
    {
      int R0;
      {
        int a0 = 0;
        R0 = 89 + a0;
      }
      R0;
    } break;
    case 2: /* foo(2) is inlined below */
#pragma spf assert nomacro
    {
      int R2;
      {
        int a2 = 2;
        R2 = 89 + a2;
      }
      R2;
    } break;
    case 3: /* foo(34) is inlined below */
#pragma spf assert nomacro
    {
      int R1;
      {
        int a1 = 34;
        R1 = 89 + a1;
      }
      R1;
    } break;
    }
  }

  return 0;
}