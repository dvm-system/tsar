int foo() { return 91; }

int main() {
  int x = 0, y;

  {
    /* foo() is inlined below */
    int R0;
#pragma spf assert nomacro
    { R0 = 91; }
    x += R0;

    /* foo() is inlined below */
    int R1;
#pragma spf assert nomacro
    { R1 = 91; }
    y = 2 * R1;
  }

  return 0;
}
