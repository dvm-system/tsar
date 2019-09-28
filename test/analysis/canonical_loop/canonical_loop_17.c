void foo() {
  unsigned X = 1;
  for (long I = 0; 10 > I; I = I + X);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_17.c:3:3 is semantically canonical
