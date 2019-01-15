int foo() { return 100; }

int main() {
  int i, j = 0;

  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 100; }
  for (i = R0; i > 0; i--) {
    j++;
  }

  return 0;
}
