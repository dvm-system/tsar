int foo() { return 91; }

int main() {
  int x = 0, y;

  {
    /* foo() is inlined below */
#pragma spf assert nomacro
    int R1;
    { R1 = 91; } /* foo() is inlined below */
#pragma spf assert nomacro
    int R0;
    { R0 = 91; }
    x = R0 + 2 * fR1;
  }

  return 0;
}