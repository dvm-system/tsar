int main() {
  unsigned I, X;
  for (I = 0; I < 4000000000; ++I)
    X = I;
  return 0;
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'main':
//CHECK: loop at canonical_loop_21.c:3:3 is semantically canonical
