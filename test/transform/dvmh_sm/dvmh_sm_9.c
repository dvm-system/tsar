double A[100][100][100];

void foo() {
  for (int I = 3; I < 100; ++I)
    for (int J = 0; J < 98; ++J)
      for (int K = 2; K < 100; ++K)
        A[I][J][K] = A[I - 3][J + 2][K - 2] + A[I][J][K];
  for (int I = 3; I < 100; ++I)
    for (int J = 2; J < 100; ++J)
      for (int K = 0; K < 98; ++K)
        A[I][J][K] = A[I - 3][J - 2][K + 2] + A[I - 2][J - 2][K + 2];
}
//CHECK: dvmh_sm_9.c:8:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 3; I < 100; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_9.c:9:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 2; J < 100; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_9.c:4:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 3; I < 100; ++I)
//CHECK:   ^
//CHECK: dvmh_sm_9.c:5:5: remark: parallel execution of loop is possible
//CHECK:     for (int J = 0; J < 98; ++J)
//CHECK:     ^
//CHECK: dvmh_sm_9.c:6:7: remark: parallel execution of loop is possible
//CHECK:       for (int K = 2; K < 100; ++K)
//CHECK:       ^
