void foo() {
  int K = 1;
  for (int I = 0; I < 10; I = I + K + 1);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_13.c:2:3 is semantically canonical
