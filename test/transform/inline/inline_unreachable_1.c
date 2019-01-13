int foo(int *I) { return *I + 1; }
int main() {
  goto L;
#pragma spf transform inline
{
  int I = 0, J;
  J = foo(&I);
}
L: return 0;
}
//CHECK: inline_unreachable_1.c:7:7: warning: disable inline expansion of unreachable call
//CHECK:   J = foo(&I);
//CHECK:       ^
//CHECK: 1 warning generated.
