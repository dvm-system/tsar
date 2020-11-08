enum { NX = 100, NY = 100 };

double A[NX][NY][5];

void bar(double B[5], double A[5]) {
  A[0] = A[0] + B[0];
  A[1] = A[1] + B[1] * 2;
  A[2] = A[2] + B[2] * 3;
  A[3] = A[3] + B[3] * 4;
  A[4] = A[4] + B[4] * 5;
}

void foo() {
  for (int I = 0; I < NX; ++I) {
    for (int J = 1; J < NY; ++J)
      bar(A[I][J - 1], A[I][J]);
    for (int J = 0; J < NY - 1; ++J)
      for (int M = 0; M < 5; ++M)
        A[I][J][M] = A[I][J][M] + A[I][J + 1][M];
  }
}
//CHECK: interproc_2.c:14:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < NX; ++I) {
//CHECK:   ^
