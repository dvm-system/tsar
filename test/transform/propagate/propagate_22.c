int foo(int Z, int *A) {
#pragma spf transform propagate
  int X, Y, Q;
  X = Y = A[Q = A[1] + 1, Z];
  return X + Y + Z;
}
//CHECK: 
