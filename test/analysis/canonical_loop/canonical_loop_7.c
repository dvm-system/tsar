void foo() {
  for (int I = 0; I < I + 1 ; ++I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_7.c:2:3 is syntactically canonical
