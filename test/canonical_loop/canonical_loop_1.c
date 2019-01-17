void foo() {
  for (int I = 0; I < 10; ++I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_1.c:2:3 is semantically canonical
