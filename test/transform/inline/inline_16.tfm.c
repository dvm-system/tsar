int M() { return 78; }

int foo(int a) { return a + 567; }

int main() {
  int x = 0;

  /* M() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 78; }
  /* foo(
                  M()) is inlined below */
  int R1;
#pragma spf assert nomacro
  {
    int a0 = R0;

    R1 = a0 + 567;
  }
  x += R1;
  return 0;
}
