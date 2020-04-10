void foo() {
  int X = 10;
  for (int I = 0; I < X; ++I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_9.c:3:3 is semantically canonical
