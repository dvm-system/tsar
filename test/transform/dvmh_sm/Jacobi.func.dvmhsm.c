//===--- Jacobi.c --------- Jacobi Iterative Method ---------------*- C -*-===//
//
// This file implements Jacobi iterative method which is an iterative method
// used to solve partial differential equations.
//
//===----------------------------------------------------------------------===//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define Max(A, B) ((A) > (B) ? (A) : (B))

#define MAXEPS 0.5

void init(int L, double (*restrict A)[L], double (*restrict B)[L]) {
#pragma dvm parallel([I]) tie(A[I][], B[I][])
  for (int I = 0; I < L; ++I)
    for (int J = 0; J < L; ++J) {
      A[I][J] = 0;
      if (I == 0 || J == 0 || I == L - 1 || J == L - 1)
        B[I][J] = 0;
      else
        B[I][J] = 3 + I + J;
    }
}

double iter(int L, double (*restrict A)[L], double (*restrict B)[L]) {
  double Eps = 0;
#pragma dvm parallel([I]) tie(A[I][], B[I][]) reduction(max(Eps))
  for (int I = 1; I < L - 1; ++I)
    for (int J = 1; J < L - 1; ++J) {
      double Tmp = fabs(B[I][J] - A[I][J]);
      Eps = Max(Tmp, Eps);
      A[I][J] = B[I][J];
    }
#pragma dvm parallel([I]) tie(A[I][], B[I][])
  for (int I = 1; I < L - 1; ++I)
    for (int J = 1; J < L - 1; ++J)
      B[I][J] = (A[I - 1][J] + A[I][J - 1] + A[I][J + 1] + A[I + 1][J]) / 4.0;
  return Eps;
}

int main(int Argc, char **Argv) {
  int L = atoi(Argv[1]);
  int ITMAX = atoi(Argv[2]);
  double(*A)[L] = malloc(L * L * sizeof(double));
  double(*B)[L] = malloc(L * L * sizeof(double));
  init(L, A, B);
  for (int It = 1; It <= ITMAX; ++It) {
    double Eps = iter(L, A, B);
    printf("It=%4i   Eps=%e\n", It, Eps);
    if (Eps < MAXEPS)
      break;
  }
  free(A);
  free(B);
  return 0;
}
