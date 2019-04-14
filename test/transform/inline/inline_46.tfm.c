int foo() { return 95; }

int main() {
  int x = 56;

  if (x > 100) {

    /* foo() is inlined below */
    int R0;
#pragma spf assert nomacro
    { R0 = 95; }
    x += R0;
  } else
    x -= foo();

  return 0;
}
