int foo() { return 5; }
int bar(int X) { return X; }

int main() {
  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 5; }
  /* bar(foo()) is inlined below */
  int R1;
#pragma spf assert nomacro
  {
    int X0 = R0;
    R1 = X0;
  }
  /* foo() is inlined below */
  int R2;
#pragma spf assert nomacro
  { R2 = 5; }
  int X = R1 + R2;

  return 0;
}
