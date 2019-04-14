int f1() { return 0; }

void f(int I) {

  {
    if (I == 1) {
    } else /* f1() is inlined below */
    {
      int R0;
#pragma spf assert nomacro
      { R0 = 0; }
      if (R0) {
      } else {
      }
    }
  }
}
