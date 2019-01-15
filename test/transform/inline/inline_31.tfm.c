#define M
#ifdef M
// We do not check that function definition is placed under condition.
// So, inlining is valid in this case.
int foo() { return 34; }
#endif

int main() {
  int x;

  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 34; }
  x = R0;

  return 0;
}
