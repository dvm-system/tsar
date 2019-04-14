int foo() { return 34; }
int main() {
  int x;
#ifdef INL

#endif
  /* foo() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 34; }
  x = R0;
  return 0;
}
