int foo() { return 91; }

int main() {
  int x = 0;

  {
    {
      /* foo() is inlined below */
      int R0;
#pragma spf assert nomacro
      { R0 = 91; }
      x += R0;
    }
  }

  return 0;
}
