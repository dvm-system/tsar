enum { N = 100 };

double A[N];
void foo() {
#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel([I]) tie(A[-I])
    for (int I = 1; I <= N; ++I) {
      A[N - I] = I;
    }
  }
#pragma dvm get_actual(A)
}
