int foo() { return 95; }

int main() {
  int x = 56;

  if (x > 100) {
    /* foo() is inlined below */
#pragma spf assert nomacro
    int R0;
    { R0 = 95; }
    x += R0;
  } else
    x -= foo();

  return 0;
}