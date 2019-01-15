int foo() {
  int x = 45;
  return x;
}

int foo_1() {
  int x = 9;
  return x;
}

int main() {
  int i = 0;

  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int x = 45;
    R0 = x;
  }
  /* foo_1() is inlined below */
  int R1;
#pragma spf assert nomacro
  {
    int x = 9;
    R1 = x;
  }
  i = R0 + R1;

  return 0;
}
