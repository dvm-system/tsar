int foo() { return 95; }

int main() {
  int x = 56;

  {
    if (x > 100) {

      /* foo() is inlined below */
      int R0;
#pragma spf assert nomacro
      { R0 = 95; }
      x += R0;
    } else
    /* foo() is inlined below */
    {
      int R1;
#pragma spf assert nomacro
      { R1 = 95; }
      x -= R1;
    }
  }
  return 0;
}
