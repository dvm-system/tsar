int foo(int *I) { return *I + 1; }

int main() {
  int I = 0, J;
  if (0) {
#pragma spf transform inline
    J = foo(&I);
  }
  return 0;
}
//CHECK: inline_unreachable_2.c:7:9: warning: disable inline expansion of unreachable call
//CHECK:     J = foo(&I);
//CHECK:         ^
//CHECK: 1 warning generated.
