double A[100];

void foo() {
  for (int I = 98; I >= 0; --I)
    A[I] = A[I + 1];
}
//CHECK: dvmh_sm_7.c:4:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 98; I >= 0; --I)
//CHECK:   ^
