void foo() {
  int I, *X = &I;
  for (I = 0; I < 10; I = I + *X);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_6.c:3:3 is syntactically canonical
