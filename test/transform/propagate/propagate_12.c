float A[10][20];
float *P;

int foo(int I, int J) {
#pragma spf transform propagate
{
  int X = I;
  int Y = J;
  int Z = 0;
  A[X][Y] = I + J + Z;
  Z = 1;
  *(Z + P + X + Y) = Z;
  return ((X = X + 1) > 0) ? X : Z;
}
}
//CHECK: 
