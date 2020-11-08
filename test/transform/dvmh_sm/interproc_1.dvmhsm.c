enum { NX = 100, NY = 100 };

double A[NX][NY][5];

void bar(double A[5]) {
  A[0] = 0;
  A[1] = 1;
  A[2] = 2;
  A[3] = 3;
  A[4] = 4;
}

void foo() {
#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel([I]) tie(A[I][][])
    for (int I = 0; I < NX; ++I) {
      for (int J = 0; J < NY; ++J)
        bar(A[I][J]);
      for (int J = 0; J < NY - 1; ++J)
        for (int M = 0; M < 5; ++M)
          A[I][J][M] = A[I][J][M] + A[I][J + 1][M];
    }
  }
#pragma dvm get_actual(A)
}
