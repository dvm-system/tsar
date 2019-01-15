int x = 50;

int foo() {
  int x = 72;
  return x + 40;
}

int main() {
  int a = 0;

  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int x = 72;
    R0 = x + 40;
  }
  a = x + R0;

  return 0;
}
