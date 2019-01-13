int foo() { return 5; }

void bar() {
#pragma spf transform inline
  int x = foo();
}
