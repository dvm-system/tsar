double A[100];
void foo(int N) {
#pragma dvm actual(A, N)
#pragma dvm region in(A, N)out(A)
  {
#pragma dvm parallel([I]) tie(A[I])
    for (int I = 0; I < N; ++I) {
      A[I] = I;
    }
  }
#pragma dvm get_actual(A)
}
