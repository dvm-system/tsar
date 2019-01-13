void f() {}

void f1() {
  #pragma spf transform inline
  f();
}
