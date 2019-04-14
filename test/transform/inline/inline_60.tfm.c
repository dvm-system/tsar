int foo(int A) { return 89 + A; }

int main(int Argc, char **Argv) {
  switch (Argc) {
  case 1:

    /* foo(0) is inlined below */
    {
      int R0;
#pragma spf assert nomacro
      {
        int A0 = 0;
        R0 = 89 + A0;
      }
      R0;
    }
    break;
  case 2:
    foo(2);
    break;
  case 3:
    foo(34);
    break;
  }
  return 0;
}
