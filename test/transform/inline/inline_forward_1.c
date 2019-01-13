int f();

void f1() {
#pragma spf transform inline
  f();
}


int A;

int f(int A) {
  return A;
}
//CHECK: inline_forward_1.c:5:3: warning: disable inline expansion
//CHECK:   f();
//CHECK:   ^
//CHECK: inline_forward_1.c:11:1: note: number of formal parameters differs from a number of call arguments
//CHECK: int f(int A) {
//CHECK: ^
//CHECK: 1 warning generated.
