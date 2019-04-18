int foo() {
#pragma spf transform propagate
  int A[3], X, Y, Z;
  for (int I = 0; I < 3; ++I)
    A[I] = I;
  X = A[0];
  Y = A[1];
  Z = A[2];
  return X + Y + Z;
}
//CHECK: 
