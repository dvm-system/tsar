int M() { return 78; }

int main() {
  int x = 0;

  /* M() is inlined below */
#pragma spf assert nomacro
  int R0;
  { R0 = 78; }
  x += R0;
  return 0;
}