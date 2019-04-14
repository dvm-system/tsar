int f(int *X);

int g(int *X) { return (*X = *X + 1); }

int main() {
  int X;
  // It is not possible to inline f() because its body is not available.
  // However, C stanard does not determine order of calls execution inside
  // a single expression. So, it is valid to perform inlining in this case.

  /* g(&X) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int *X0 = &X;
    R0 = (*X0 = *X0 + 1);
  }
  return f(&X) + R0;
}
