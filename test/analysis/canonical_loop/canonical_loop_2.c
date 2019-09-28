void foo() {
  int I, J;
  for (I = 0; I < J; ++I)
    ++J;
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_2.c:3:3 is syntactically canonical
