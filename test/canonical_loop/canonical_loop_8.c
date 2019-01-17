void foo() {
  int I, *X = &I;
  for (I = 0; I < *X ; ++I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_8.c:3:3 is syntactically canonical
