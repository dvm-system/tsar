int foo() { return 100; }

int main() {
  int i, j = 0;

  /* foo() is inlined below */
#pragma spf assert nomacro
  int R0;
  { R0 = 100; }
  for (i = R0; i > 0; i--) {
    j++;
  };

  return 0;
}