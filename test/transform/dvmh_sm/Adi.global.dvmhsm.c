//===--- Adi.c ----  Alternating Direction Implicit ---------------*- C -*-===//
//
// This file implements the Alternating Direction Implicit(ADI) method which is
// an iterative method used to solve partial differential equations.
//
//===----------------------------------------------------------------------===//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX(A, b) ((A) > (b) ? (A) : (b))

#define NX 384
#define NY 384
#define NZ 384

void init();
double iter();

double A[NX][NY][NZ];

int main(int Argc, char *Argv[]) {
  double MaxEps, Eps;
  int It, ItMax, I, J, K;
  MaxEps = 0.01;
  ItMax = 100;
  init();
  for (It = 1; It <= ItMax; It++) {
    Eps = iter();
    printf(" IT = %4i   EPS = %14.7E\n", It, Eps);
    if (Eps < MaxEps)
      break;
  }
  printf(" ADI Benchmark Completed.\n");
  printf(" Size            = %4d x %4d x %4d\n", NX, NY, NZ);
  printf(" Iterations      =       %12d\n", ItMax);
  printf(" Operation type  =   double precision\n");
  printf(" Verification    =       %12s\n",
         (fabs(Eps - 0.07249074) < 1e-6 ? "SUCCESSFUL" : "UNSUCCESSFUL"));
  printf(" END OF ADI Benchmark\n");
  return 0;
}

void init() {
  int I, J, K;
#pragma dvm actual(A, I)
#pragma dvm region in(A, I)out(A, I) local(J, K)
  {
#pragma dvm parallel([I][J][K]) tie(A[I][J][K])
    for (I = 0; I < NX; I++)
      for (J = 0; J < NY; J++)
        for (K = 0; K < NZ; K++)
          if (K == 0 || K == NZ - 1 || J == 0 || J == NY - 1 || I == 0 ||
              I == NX - 1)
            A[I][J][K] =
                10.0 * I / (NX - 1) + 10.0 * J / (NY - 1) + 10.0 * K / (NZ - 1);
          else
            A[I][J][K] = 0;
  }
#pragma dvm get_actual(A, I)
}

double iter() {
  int I, J, K;
  double Eps = 0;
#pragma dvm actual(A, Eps, I)
#pragma dvm region in(A, Eps, I)out(A, Eps, I) local(J, K)
  {
#pragma dvm parallel([I][J][K]) tie(A[I][J][K]) across(A [1:1] [0:0] [0:0])
    for (I = 1; I < NX - 1; I++)
      for (J = 1; J < NY - 1; J++)
        for (K = 1; K < NZ - 1; K++)
          A[I][J][K] = (A[I - 1][J][K] + A[I + 1][J][K]) / 2;
#pragma dvm parallel([I][J][K]) tie(A[I][J][K]) across(A [0:0] [1:1] [0:0])
    for (I = 1; I < NX - 1; I++)
      for (J = 1; J < NY - 1; J++)
        for (K = 1; K < NZ - 1; K++)
          A[I][J][K] = (A[I][J - 1][K] + A[I][J + 1][K]) / 2;
#pragma dvm parallel([I][J][K]) tie(A[I][J][K]) across(A [0:0] [0:0] [1:1])    \
    reduction(max(Eps))
    for (I = 1; I < NX - 1; I++)
      for (J = 1; J < NY - 1; J++)
        for (K = 1; K < NZ - 1; K++) {
          double Tmp1 = (A[I][J][K - 1] + A[I][J][K + 1]) / 2;
          double Tmp2 = fabs(A[I][J][K] - Tmp1);
          Eps = MAX(Eps, Tmp2);
          A[I][J][K] = Tmp1;
        }
  }
#pragma dvm get_actual(A, Eps, I)

  return Eps;
}
