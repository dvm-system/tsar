void foo(int N, double (*A)[N/2*2+2]);

void bar() {
  double A[10][10];
  #pragma spf transform inline
  foo(8, A);
}
//CHECK: inline_param_5.c:1:28: warning: import of variable-length array is partially supported
//CHECK: void foo(int N, double (*A)[N/2*2+2]);
//CHECK:                            ^
//CHECK: 1 warning generated.
//CHECK-1: 
