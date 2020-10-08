enum { N = 100 };

double A[N][N], B[N];

int main() {
  int I, J;
  double S = 0;
  for (I = 0; I < N; ++I)
    for (J = 0; J < N; ++J)
      A[I][J] = I * 2.3 * J;
  for (I = 1; I < N; ++I)
    B[I] =  A[I - 1][I - 1];
  B[0] = 1;
  for (I = 0; I < N; ++I)
    for (J = 0; J < N; ++J)
      S = S + A[I][J] * B[J];
  return S;
}
//CHECK: dvmh_sm_3.c:14:3: remark: parallel execution of loop is possible
//CHECK:   for (I = 0; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_3.c:15:5: remark: parallel execution of loop is possible
//CHECK:     for (J = 0; J < N; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_3.c:11:3: remark: parallel execution of loop is possible
//CHECK:   for (I = 1; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_3.c:8:3: remark: parallel execution of loop is possible
//CHECK:   for (I = 0; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_3.c:9:5: remark: parallel execution of loop is possible
//CHECK:     for (J = 0; J < N; ++J)
//CHECK:     ^
