void foo() {
  int I;
  for (I = 0; I < 10; I = I + I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at canonical_loop_4.c:3:3 is syntactically canonical
