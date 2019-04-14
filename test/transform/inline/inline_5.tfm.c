int f() { return 0; }

void f1() {
  for (int I = 0; I < 10; ++I) {

    /* f() is inlined below */
    int R1;
#pragma spf assert nomacro
    { R1 = 0; }
    R1;
  }
}

void f2() {

  for (int I = 0; I < 10; I = I + f())
    ;
}

void f3() {

  for (int I = 0; I < f(); ++I)
    ;
}

void f4() {

  /* f() is inlined below */
  int R0;
#pragma spf assert nomacro
  { R0 = 0; }
  for (int I = R0; I < 10; ++I)
    ;
}
