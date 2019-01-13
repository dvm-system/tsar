void f(int *x) {
 *x = *x + 1;
}

int f1(int *x) {
  #pragma spf transform inline
  return *x = 1, f(x), *x;
}
//CHECK: inline_9.c:7:18: warning: disable inline expansion in comma operator
//CHECK:   return *x = 1, f(x), *x;
//CHECK:                  ^
//CHECK: 1 warning generated.
