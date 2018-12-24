int foo() { return 95; }

int main() {
  int x = 56;

  {
    if (x > 100) {

      /* foo() is inlined below */
#pragma spf assert nomacro
      int R1;
      { R1 = 95; }
      x += R1;
    } else
    /* foo() is inlined below */
#pragma spf assert nomacro
    {
      int R0;
      { R0 = 95; }
      x -= R0;
    }
  }
  return 0;
}