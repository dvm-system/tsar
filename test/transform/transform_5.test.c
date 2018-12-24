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

  /* foo_1() is inlined below */
#pragma spf assert nomacro
  int R1;
  {
    int x = 9;
    R1 = x;
  } /* foo() is inlined below */
#pragma spf assert nomacro
  int R0;
  {
    int x = 45;
    R0 = x;
  }
  i = R0 + fR1;
  return 0;
}