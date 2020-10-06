void foo(int N, double (*A)[N/2*2+2]);

void bar() {
  double A[10][10];
  #pragma spf transform inline
  foo(8, A);
}
//CHECK: 
//CHECK-1: 
