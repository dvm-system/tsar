int foo(int X, int Z) {
#pragma spf transform propagate
  int Y = X;
  int Q = Z;
  Y = Q;
  return Y;
}
//CHECK: 
