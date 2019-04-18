void bar1();
void bar2();

void foo() {

  void (*f)() = bar1;
  bar1();
  f = bar2;
  bar2();
}
