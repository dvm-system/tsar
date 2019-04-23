float A[10][10];

void foo(int J) {
#pragma spf transform propagate
  float *B;
  B = A[J];
  for (int I = 0; I < 10; ++I) {
    B[I] = J;
  }
}
//CHECK: 
