#define M
#ifdef M
int foo() { return 34; }
#endif

int main() {
  int x;

  /* foo() is inlined below */
#pragma spf assert nomacro
  int R0;
  { R0 = 34; }
  x = R0;

  return 0;
}