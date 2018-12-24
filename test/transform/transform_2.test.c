int M() { return 78; }

int foo(int a) { return a + 567; }

int main() {
  int x = 0;

  /* M() is inlined below */
#pragma spf assert nomacro
  int R1;
  {
    R1 = 78;
  } /* foo(
                   M()) is inlined below */
#pragma spf assert nomacro
  int R0;
  {
    int a0 = M();

    R0 = a0 + 567;
  }
  x += R1;
  ;
  return 0;
}