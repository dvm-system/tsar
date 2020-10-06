int I;
void foo() {
  for (I = 0; I < 10; ++I);
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at global_3.c:3:3 is semantically canonical
