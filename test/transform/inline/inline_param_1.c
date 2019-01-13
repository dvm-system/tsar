void f(int *A, int I) {
  A[I] = 10;
}

void g() {
  int A[10];
#pragma spf transform inline
  f(A, 1);
}
//CHECK: 
