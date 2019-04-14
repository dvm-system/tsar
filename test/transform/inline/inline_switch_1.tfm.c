int f1() { return 0; }

void f(int X) {

  {
    switch (X) {
    case 1: /* f1() is inlined below */
      int R0;
#pragma spf assert nomacro
      { R0 = 0; }
      { R0; }
      break;
    case 2: /* f1() is inlined below */
    {
      int R1;
#pragma spf assert nomacro
      { R1 = 0; }
      R1;
    } break;
    default: /* f1() is inlined below */
    {
      int R2;
#pragma spf assert nomacro
      { R2 = 0; }
      R2;
    } break;
    }
  }
}
