enum { N = 100 };

double A[N], B[N];

int main() {
  double S = 1.;
#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel on([I]) tie(A[I])
    for (int I = 0; I < N; ++I)
      A[I] = I * 2.3;
  }
#pragma dvm get_actual(A)

#pragma dvm actual(A, B)
#pragma dvm region in(A, B)out(B)
  {
#pragma dvm parallel on([I]) tie(A[I], B[I])
    for (int I = 1; I < N; ++I)
      B[I] = A[I - 1];
  }
#pragma dvm get_actual(B)

#pragma dvm actual(A, B, S)
#pragma dvm region in(A, B, S)out(S)
  {
#pragma dvm parallel on([I]) tie(A[I], B[I]) reduction(product(S))
    for (int I = 0; I < N / 2; I += 2)
      S = S * A[I] * B[I + 1];
  }
#pragma dvm get_actual(S)

  return S;
}
