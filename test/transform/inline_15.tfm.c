int M() { return 78; }

int main() {
  int x = 0;

  /* M() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 78; }
  x += R0;
  return 0;
}
