int foo() { return 91; }

int main() {
  int x = 0;

  {
    /* foo() is inlined below */
#pragma spf assert nomacro
    int R0;
    { R0 = 91; }
    x += R0;
  }

  return 0;
}