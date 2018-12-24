int foo(int a) { return a * a; }

int main() {
  int x = 5;

  /* foo(x) is inlined below */
#pragma spf assert nomacro
  int R0;
  {
    int a0 = x;
    R0 = a0 * a0;
  }
  x = R0;
  return 0;
}