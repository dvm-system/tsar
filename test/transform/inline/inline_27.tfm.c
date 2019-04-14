#include <stdlib.h>
#include <stdio.h>

int *foo1(int N, int **A) {
  *A = (int *)malloc(N * sizeof(int));
  for (int I = 0; I < N; ++I)
    (*A)[I] = I;
  return *A;
}

int foo2(int N, int *A) {
  int S = 0;
  for (int I = 0; I < N; ++I)
    S = S + A[I];
  return S;
}

int main() {
  int *A, N, S;

  {
    /* foo1(N, &A) is inlined below */
    int *R0;
#pragma spf assert nomacro
    {
      int N0 = N;
      int **A0 = &A;
      *A0 = (int *)malloc(N0 * sizeof(int));
      for (int I = 0; I < N0; ++I)
        (*A0)[I] = I;
      R0 = *A0;
    }
    /* foo2(N, foo1(N, &A)) is inlined below */
    int R1;
#pragma spf assert nomacro
    {
      int N1 = N;
      int *A1 = R0;
      int S = 0;
      for (int I = 0; I < N1; ++I)
        S = S + A1[I];
      R1 = S;
    }
    S = R1;
  }
  printf("S=%d\n", S);
  return 0;
}
