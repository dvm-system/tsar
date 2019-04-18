int A[10];

void foo(int I) {
#pragma spf transform propagate
  int X = I;
  A[X = X + 1, X + 1] = I;
}
//CHECK: 
