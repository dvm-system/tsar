int f1();

int f2() {
#pragma spf transform inline
  return f1();
}

int f1() {
#pragma spf transform inline
  return f2() + 1;
}


//CHECK: inline_recursive_1.c:3:5: warning: disable inline expansion of recursive function
//CHECK: int f2() {
//CHECK:     ^
//CHECK: inline_recursive_1.c:8:5: warning: disable inline expansion of recursive function
//CHECK: int f1() {
//CHECK:     ^
//CHECK: 2 warnings generated.
