void bar1();
void bar2();

void foo() {
#pragma spf transform propagate
  void (*f)() = bar1;
  f();
  f = bar2;
  f();
}
//CHECK: 
