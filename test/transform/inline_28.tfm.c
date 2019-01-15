int foo(int a) {
  if (a > 0)
    return foo(a - 1);
  else
    return 0;
}

int main() {

  /* foo(20) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int a0 = 20;
    if (a0 > 0) {
      R0 = foo(a0 - 1);
      goto L0;
    } else {
      R0 = 0;
      goto L0;
    }
  }
L0:;
  int x = R0;

  return 0;
}
