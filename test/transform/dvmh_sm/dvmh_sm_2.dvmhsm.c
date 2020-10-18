enum { N = 100 };

double A[N], B[N];

int main() {
  double S = 1.;
#pragma dvm actual(A, B, S)
#pragma dvm region in(A, B, S)out(A, B, S)
  {
#pragma dvm parallel([I]) tie(A[I])
    for (int I = 0; I < N; ++I)
      A[I] = I * 2.3;
#pragma dvm parallel([I]) tie(A[I], B[I])
    for (int I = 1; I < N; ++I)
      B[I] = A[I - 1];
#pragma dvm parallel([I]) tie(A[I], B[I]) reduction(product(S))
    for (int I = 0; I < N / 2; I += 2)
      S = S * A[I] * B[I + 1];
  }
#pragma dvm get_actual(A, B, S)

  return S;
}
