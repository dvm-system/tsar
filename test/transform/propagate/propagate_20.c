
float foo(int N, float (*A)[N]) {
#pragma spf transform propagate
  int X;
  X = N - 1;
  return (A[X][X - 1] = 2.1);
}
//CHECK: 
