double A[100];

void foo() {
  int X = -1;
  for (int I = 98; I >= 0; I = I + X)
    A[I] = A[I + 1];
}
//CHECK: dvmh_sm_8.c:5:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 98; I >= 0; I = I + X)
//CHECK:   ^
//CHECK: dvmh_sm_8.c:5:3: warning: unable to create parallel directive
//CHECK: dvmh_sm_8.c:5:3: note: unable to implement pipeline execution for a loop with unknown step
//CHECK: 1 warning generated.
