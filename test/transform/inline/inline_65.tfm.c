void bar1() {}
void bar2(double x) {}

void foo() {
  double val;

  {
    /* bar1() is inlined below */
#pragma spf assert nomacro
    {}

    /* bar2(val) is inlined below */
#pragma spf assert nomacro
    { double x1 = val; }
  }
}

void foo1() {

  /* foo() is inlined below */
#pragma spf assert nomacro
  {
    double val;
    {
      /* bar1() is inlined below */
#pragma spf assert nomacro
      {}

      /* bar2(val) is inlined below */
#pragma spf assert nomacro
      { double x0 = val; }
    }
  }
}
