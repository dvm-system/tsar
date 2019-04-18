void foo(int X, int (*A)[X]) {
#pragma spf transform propagate
  int Y = X;
  A[Y][Y] = Y;
}
//CHECK: 
