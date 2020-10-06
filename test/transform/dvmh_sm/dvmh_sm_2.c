enum { N = 100 };

double A[N], B[N];

int main() {
  double S = 1.;
  for (int I = 0; I < N; ++I)
    A[I] = I * 2.3;
  for (int I = 1; I < N; ++I)
    B[I] =  A[I - 1];
  for (int I = 0; I < N / 2; I += 2)
    S = S * A[I] * B[I + 1];
  return S;
}
//CHECK: dvmh_sm_2.c:11:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N / 2; I += 2)
//CHECK:   ^
//CHECK: dvmh_sm_2.c:9:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 1; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_2.c:7:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I)
//CHECK:   ^
