int foo() { return 5; }

void bar() {
  /* foo() is inlined below */
#pragma spf assert nomacro
  {
    int R0;
    { R0 = 5; }
    int x = R0;
    ;
  }
}
