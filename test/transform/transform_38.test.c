int foo() { return 42; }

int main() {
  int i, j, k, x = 0;

  {
    for (i = 0; i < 32; i++) {
      /* foo() is inlined below */
#pragma spf assert nomacro
      int R0;
      { R0 = 42; }
      for (j = R0; j < 32; j++) {
        for (k = 0; k < 32; k++) {

          x += 9;
        }
      };
    }
  }
  return 0;
} 