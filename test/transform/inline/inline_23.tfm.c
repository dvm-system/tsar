int foo(int a1, int a2) { return a1 * a1 + a2 + 9; }

int main() {
  int k = 3, p = 32;

  /* foo(k, p) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int a10 = k;
    int a20 = p;
    R0 = a10 * a10 + a20 + 9;
  }
  int j = R0;

  return 0;
}
