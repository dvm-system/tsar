void foo() {
  int X = 1;
  for (int I = 10; I > 0; I = I - X);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_15.c:3:3 is semantically canonical
