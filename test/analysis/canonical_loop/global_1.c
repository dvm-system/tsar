static int FirstCol, LastCol;
static int Data[100];

int foo(int S) {
  int I;
  for (I = 0; I < LastCol - FirstCol + 1; ++I) {
    S = S * Data[I];
  }
  return S;
}
//CHECK: Printing analysis 'Canonical Form Loop Analysis' for function 'foo':
//CHECK: loop at global_1.c:6:3 is semantically canonical
