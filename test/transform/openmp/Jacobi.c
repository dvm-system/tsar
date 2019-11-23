//===--- Jacobi.c --------- Jacobi Iterative Method ---------------*- C -*-===//
//
// This file implements Jacobi iterative method which is an iterative method
// used to solve partial differential equations.
//
//===----------------------------------------------------------------------===//

#include <math.h>
#include <stdio.h>

#define Max(A, B) ((A) > (B) ? (A) : (B))

#define L 8
#define ITMAX 10
#define MAXEPS 0.5

double A[L][L];
double B[L][L];

int main() {
  for (int I = 0; I < L; ++I)
    for (int J = 0; J < L; ++J) {
      A[I][J] = 0;
      if (I == 0 || J == 0 || I == L - 1 || J == L - 1)
        B[I][J] = 0;
      else
        B[I][J] = 3 + I + J;
    }
  for (int It = 1; It <= ITMAX; ++It) {
    double Eps = 0;
    for (int I = 1; I < L - 1; ++I)
      for (int J = 1; J < L - 1; ++J) {
        double Tmp = fabs(B[I][J] - A[I][J]); 
        Eps = Max(Tmp, Eps);
        A[I][J] = B[I][J];
      }
    for (int I = 1; I < L - 1; ++I)
      for (int J = 1; J < L - 1; ++J)
        B[I][J] = (A[I - 1][J] + A[I][J - 1] + A[I][J + 1] + A[I + 1][J]) / 4.0;
    printf("It=%4i   Eps=%e\n", It, Eps);
    if (Eps < MAXEPS)
      break;
  }
  return 0;
}
//CHECK: Jacobi.c:31:5: remark: parallel execution of loop is possible
//CHECK:     for (int I = 1; I < L - 1; ++I)
//CHECK:     ^
//CHECK: Jacobi.c:37:5: remark: parallel execution of loop is possible
//CHECK:     for (int I = 1; I < L - 1; ++I)
//CHECK:     ^
//CHECK: Jacobi.c:21:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < L; ++I)
//CHECK:   ^
