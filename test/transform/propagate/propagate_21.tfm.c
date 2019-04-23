float A[10][10];

void foo(int J) {

  float *B;
  B = A[J];
  for (int I = 0; I < 10; ++I) {
    (A[J])[I] = J;
  }
}
