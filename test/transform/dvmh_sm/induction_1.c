enum { N = 100 };

double A[N];

int main() {
  int I, J;
  double S = 0;
  for (I = 0; I < N; ++I)
    A[I] = I;
  J = 1;
  for (I = 0; I < N - 1; ++I) {
    S = S + A[I] + A[J];
    J = J + 1;
  } 
  return S;
}
//CHECK: induction_1.c:11:3: remark: parallel execution of loop is possible
//CHECK:   for (I = 0; I < N - 1; ++I) {
//CHECK:   ^
//CHECK: induction_1.c:11:3: warning: unable to create parallel directive
//CHECK: induction_1.c:6:7: note: loop has multiple inducition variables
//CHECK:   int I, J;
//CHECK:       ^
//CHECK: induction_1.c:6:10: note: loop has multiple inducition variables
//CHECK:   int I, J;
//CHECK:          ^
//CHECK: induction_1.c:8:3: remark: parallel execution of loop is possible
//CHECK:   for (I = 0; I < N; ++I)
//CHECK:   ^
//CHECK: 1 warning generated.
