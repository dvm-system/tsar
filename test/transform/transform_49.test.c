int foo(int a) { return a--; }

int main() {
  int x = 50;

  while (x > 0) {
    /* foo(x) is inlined below */
#pragma spf assert nomacro
    int R0;
    {
      int a0 = x;
      R0 = a0--;
    }
    x = R0;
  }

  return 0;
}