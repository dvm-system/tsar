int foo() { return 42; }

int main() {
  int i, j, k, x = 0;

  {
    for (i = 0; i < 32; i++) {
      for (j = 0; j < 32; j++) {
        for (k = 0; k < 32; k++) {

          /* foo() is inlined below */
          int R0;
#pragma spf assert nomacro
          { R0 = 42; }
          x += R0;
        }
      }
    }
  }
  return 0;
}
