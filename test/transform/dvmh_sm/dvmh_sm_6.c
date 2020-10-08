#include <stdio.h>

#define N 10

long A[N][N][2];

int main() {
  long S = 0;
  for (int I = 0; I < N; ++I)
    for (int J = 0; J < N; ++J)
      for (int K = 0; K < 2; ++K)
        A[I][J][K] = I + J + K;
  for (int I = 1; I < N; ++I)
    for (int J = 1; J < N; ++J)
      for (int K = 0; K < 2; ++K)
        A[I][J][K] = A[I - 1][J][K] + A[I][J - 1][K] + A[I][J][K];
  for (int I = 0; I < N; ++I)
    for (int J = 0; J < N; ++J)
      for (int K = 0; K < 2; ++K)
        S += A[I][J][K];
  printf("Sum = %ld\n", S);
  return 0;
}
//CHECK: dvmh_sm_6.c:17:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_6.c:18:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 0; J < N; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_6.c:19:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 0; K < 2; ++K)
//CHECK:       ^
//CHECK: dvmh_sm_6.c:13:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 1; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_6.c:14:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 1; J < N; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_6.c:15:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 0; K < 2; ++K)
//CHECK:       ^
//CHECK: dvmh_sm_6.c:9:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_6.c:10:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 0; J < N; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_6.c:11:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 0; K < 2; ++K)
//CHECK:       ^
