void foo() {
  for (int I = 10; I > 0; I = I - 1);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_13.c:2:3 is semantically canonical
