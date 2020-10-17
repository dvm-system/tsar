double A[100][100][100];

void foo() {
#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel([I][J][K]) tie(A[-I][-J][K]) across(A [0:3] [0:0] [1:0])
    for (int I = 99; I >= 3; --I)
      for (int J = 97; J >= 0; --J)
        for (int K = 2; K < 100; ++K)
          A[I][J][K] = A[I + 3][J + 2][K - 2] + A[I][J][K - 1];
  }
#pragma dvm get_actual(A)

#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel([I][J][K]) tie(A[I][J][-K]) across(A [3:0] [0:0] [0:0])
    for (int I = 3; I < 100; ++I)
      for (int J = 2; J < 100; ++J)
        for (int K = 97; K >= 0; --K)
          A[I][J][K] = A[I - 3][J - 2][K + 2] + A[I - 2][J - 2][K + 2];
  }
#pragma dvm get_actual(A)
}
