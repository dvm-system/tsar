int foo(int a) { return 89 + a; }

int main() {
  int n = 7;

  {
    switch (n) {

    case 1:

      /* foo(0) is inlined below */
      {
        int R0;
#pragma spf assert nomacro
        {
          int a0 = 0;
          R0 = 89 + a0;
        }
        R0;
      }
      break;
    case 2: /* foo(2) is inlined below */
    {
      int R1;
#pragma spf assert nomacro
      {
        int a1 = 2;
        R1 = 89 + a1;
      }
      R1;
    } break;
    case 3: /* foo(34) is inlined below */
    {
      int R2;
#pragma spf assert nomacro
      {
        int a2 = 34;
        R2 = 89 + a2;
      }
      R2;
    } break;
    }
  }

  return 0;
}
