void foo() {
  for (long I = 0; 10 > I; I = I + (unsigned)1);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_18.c:2:3 is semantically canonical
