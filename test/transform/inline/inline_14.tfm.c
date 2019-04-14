int f() { return 5; }
int main() {

  /* f() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 5; }
  struct A {
    int X;
  } A1 = {.X = R0};
  struct A A2;
}
