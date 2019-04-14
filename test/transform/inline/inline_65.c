void bar1() { }
void bar2(double x) {}

void foo() {
  double val;
  #pragma spf transform inline
  {
    bar1();
    bar2(val);
  }
}

void foo1() {
  #pragma spf transform inline
  foo();
}

//CHECK: 
