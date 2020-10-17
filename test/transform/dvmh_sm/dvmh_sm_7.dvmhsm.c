double A[100];

void foo() {
#pragma dvm actual(A)
#pragma dvm region in(A)out(A)
  {
#pragma dvm parallel([I]) tie(A[-I]) across(A [0:1])
    for (int I = 98; I >= 0; --I)
      A[I] = A[I + 1];
  }
#pragma dvm get_actual(A)
}
