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

void init(double (*A)[NY][NZ]);
double iter(double (*A)[NY][NZ]);

int main(int Argc, char *Argv[]) {
  double MaxEps, Eps;
  double(*A)[NY][NZ];
  int It, ItMax, I, J, K;
  MaxEps = 0.01;
  ItMax = 100;
  A = (double(*)[NY][NZ])malloc(NX * NY * NZ * sizeof(double));
  init(A);
  for (It = 1; It <= ItMax; It++) {
    Eps = iter(A);
    printf(" IT = %4i   EPS = %14.7E\n", It, Eps);
    if (Eps < MaxEps)
      break;
  }
  free(A);
  printf(" ADI Benchmark Completed.\n");
  printf(" Size            = %4d x %4d x %4d\n", NX, NY, NZ);
  printf(" Iterations      =       %12d\n", ItMax);
  printf(" Operation type  =   double precision\n");
  printf(" Verification    =       %12s\n",
         (fabs(Eps - 0.07249074) < 1e-6 ? "SUCCESSFUL" : "UNSUCCESSFUL"));
  printf(" END OF ADI Benchmark\n");
  return 0;
}

void init(double (*A)[NY][NZ]) {
  int I, J, K;
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

double iter(double(*A)[NY][NZ]) {
  int I, J, K;
  double Eps = 0;
  for (I = 1; I < NX - 1; I++)
    for (J = 1; J < NY - 1; J++)
      for (K = 1; K < NZ - 1; K++)
        A[I][J][K] = (A[I - 1][J][K] + A[I + 1][J][K]) / 2;
  for (I = 1; I < NX - 1; I++)
    for (J = 1; J < NY - 1; J++)
      for (K = 1; K < NZ - 1; K++)
        A[I][J][K] = (A[I][J - 1][K] + A[I][J + 1][K]) / 2;
  for (I = 1; I < NX - 1; I++)
    for (J = 1; J < NY - 1; J++)
      for (K = 1; K < NZ - 1; K++) {
        double Tmp1 = (A[I][J][K - 1] + A[I][J][K + 1]) / 2;
        double Tmp2 = fabs(A[I][J][K] - Tmp1);
        Eps = MAX(Eps, Tmp2);
        A[I][J][K] = Tmp1;
      }
  return Eps;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 Adi.func.c:29:3
//CHECK:    first private:
//CHECK:     <Eps:22:18, 8>
//CHECK:    dynamic private:
//CHECK:     <Eps:22:18, 8>
//CHECK:    output:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> printf():31:5
//CHECK:    anti:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> printf():31:5
//CHECK:    flow:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> printf():31:5
//CHECK:    induction:
//CHECK:     <It:24:7, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <A:23:11, 8> | <ItMax:24:11, 4> | <MaxEps:22:10, 8>
//CHECK:    redundant:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> iter():30:11 printf():31:5
//CHECK:    lock:
//CHECK:     <It:24:7, 4> | <ItMax:24:11, 4>
//CHECK:    header access:
//CHECK:     <It:24:7, 4> | <ItMax:24:11, 4>
//CHECK:    explicit access:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> printf():31:5 | <A:23:11, 8> | <Eps:22:18, 8> | <It:24:7, 4> | <ItMax:24:11, 4> | <MaxEps:22:10, 8>
//CHECK:    address access:
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <malloc():27:26,?> iter():30:11 printf():31:5
//CHECK:    explicit access (separate):
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <A:23:11, 8> <Eps:22:18, 8> <It:24:7, 4> <ItMax:24:11, 4> <MaxEps:22:10, 8> printf():31:5
//CHECK:    redundant (separate):
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> iter():30:11
//CHECK:    lock (separate):
//CHECK:     <It:24:7, 4> <ItMax:24:11, 4>
//CHECK:    address access (separate):
//CHECK:     iter():30:11 printf():31:5
//CHECK:    direct access (separate):
//CHECK:     <*A:{28:8|30:16|35:8|23:11}, ?> <A:23:11, 8> <Eps:22:18, 8> <It:24:7, 4> <ItMax:24:11, 4> <MaxEps:22:10, 8> iter():30:11 printf():31:5
//CHECK:    indirect access (separate):
//CHECK:     <malloc():27:26,?>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'init':
//CHECK:  loop at depth 1 Adi.func.c:48:3
//CHECK:    shared:
//CHECK:     <*A:46:20, ?>
//CHECK:    first private:
//CHECK:     <*A:46:20, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:46:20, ?>
//CHECK:    private:
//CHECK:     <J:47:10, 4> | <K:47:13, 4>
//CHECK:    induction:
//CHECK:     <I:47:7, 4>:[Int,0,384,1]
//CHECK:    read only:
//CHECK:     <A:46:20, 8>
//CHECK:    lock:
//CHECK:     <I:47:7, 4>
//CHECK:    header access:
//CHECK:     <I:47:7, 4>
//CHECK:    explicit access:
//CHECK:     <A:46:20, 8> | <I:47:7, 4> | <J:47:10, 4> | <K:47:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK:    lock (separate):
//CHECK:     <I:47:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:46:20, ?> <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK:   loop at depth 2 Adi.func.c:49:5
//CHECK:     shared:
//CHECK:      <*A:46:20, ?>
//CHECK:     first private:
//CHECK:      <*A:46:20, ?>
//CHECK:     dynamic private:
//CHECK:      <*A:46:20, ?>
//CHECK:     private:
//CHECK:      <K:47:13, 4>
//CHECK:     induction:
//CHECK:      <J:47:10, 4>:[Int,0,384,1]
//CHECK:     read only:
//CHECK:      <A:46:20, 8> | <I:47:7, 4>
//CHECK:     lock:
//CHECK:      <J:47:10, 4>
//CHECK:     header access:
//CHECK:      <J:47:10, 4>
//CHECK:     explicit access:
//CHECK:      <A:46:20, 8> | <I:47:7, 4> | <J:47:10, 4> | <K:47:13, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK:     lock (separate):
//CHECK:      <J:47:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:46:20, ?> <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK:    loop at depth 3 Adi.func.c:50:7
//CHECK:      shared:
//CHECK:       <*A:46:20, ?>
//CHECK:      first private:
//CHECK:       <*A:46:20, ?>
//CHECK:      dynamic private:
//CHECK:       <*A:46:20, ?>
//CHECK:      induction:
//CHECK:       <K:47:13, 4>:[Int,0,384,1]
//CHECK:      read only:
//CHECK:       <A:46:20, 8> | <I:47:7, 4> | <J:47:10, 4>
//CHECK:      lock:
//CHECK:       <K:47:13, 4>
//CHECK:      header access:
//CHECK:       <K:47:13, 4>
//CHECK:      explicit access:
//CHECK:       <A:46:20, 8> | <I:47:7, 4> | <J:47:10, 4> | <K:47:13, 4>
//CHECK:      explicit access (separate):
//CHECK:       <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK:      lock (separate):
//CHECK:       <K:47:13, 4>
//CHECK:      direct access (separate):
//CHECK:       <*A:46:20, ?> <A:46:20, 8> <I:47:7, 4> <J:47:10, 4> <K:47:13, 4>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'iter':
//CHECK:  loop at depth 1 Adi.func.c:62:3
//CHECK:    private:
//CHECK:     <J:60:10, 4> | <K:60:13, 4>
//CHECK:    anti:
//CHECK:     <*A:59:21, ?>:[1,1]
//CHECK:    flow:
//CHECK:     <*A:59:21, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:60:7, 4>:[Int,1,383,1]
//CHECK:    read only:
//CHECK:     <A:59:21, 8>
//CHECK:    lock:
//CHECK:     <I:60:7, 4>
//CHECK:    header access:
//CHECK:     <I:60:7, 4>
//CHECK:    explicit access:
//CHECK:     <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:    lock (separate):
//CHECK:     <I:60:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:   loop at depth 2 Adi.func.c:63:5
//CHECK:     shared:
//CHECK:      <*A:59:21, ?>
//CHECK:     private:
//CHECK:      <K:60:13, 4>
//CHECK:     induction:
//CHECK:      <J:60:10, 4>:[Int,1,383,1]
//CHECK:     read only:
//CHECK:      <A:59:21, 8> | <I:60:7, 4>
//CHECK:     lock:
//CHECK:      <J:60:10, 4>
//CHECK:     header access:
//CHECK:      <J:60:10, 4>
//CHECK:     explicit access:
//CHECK:      <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:     lock (separate):
//CHECK:      <J:60:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:    loop at depth 3 Adi.func.c:64:7
//CHECK:      shared:
//CHECK:       <*A:59:21, ?>
//CHECK:      induction:
//CHECK:       <K:60:13, 4>:[Int,1,383,1]
//CHECK:      read only:
//CHECK:       <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4>
//CHECK:      lock:
//CHECK:       <K:60:13, 4>
//CHECK:      header access:
//CHECK:       <K:60:13, 4>
//CHECK:      explicit access:
//CHECK:       <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:      explicit access (separate):
//CHECK:       <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:      lock (separate):
//CHECK:       <K:60:13, 4>
//CHECK:      direct access (separate):
//CHECK:       <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:  loop at depth 1 Adi.func.c:66:3
//CHECK:    shared:
//CHECK:     <*A:59:21, ?>
//CHECK:    private:
//CHECK:     <J:60:10, 4> | <K:60:13, 4>
//CHECK:    induction:
//CHECK:     <I:60:7, 4>:[Int,1,383,1]
//CHECK:    read only:
//CHECK:     <A:59:21, 8>
//CHECK:    lock:
//CHECK:     <I:60:7, 4>
//CHECK:    header access:
//CHECK:     <I:60:7, 4>
//CHECK:    explicit access:
//CHECK:     <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:    lock (separate):
//CHECK:     <I:60:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:   loop at depth 2 Adi.func.c:67:5
//CHECK:     private:
//CHECK:      <K:60:13, 4>
//CHECK:     anti:
//CHECK:      <*A:59:21, ?>:[1,1]
//CHECK:     flow:
//CHECK:      <*A:59:21, ?>:[1,1]
//CHECK:     induction:
//CHECK:      <J:60:10, 4>:[Int,1,383,1]
//CHECK:     read only:
//CHECK:      <A:59:21, 8> | <I:60:7, 4>
//CHECK:     lock:
//CHECK:      <J:60:10, 4>
//CHECK:     header access:
//CHECK:      <J:60:10, 4>
//CHECK:     explicit access:
//CHECK:      <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:     lock (separate):
//CHECK:      <J:60:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:    loop at depth 3 Adi.func.c:68:7
//CHECK:      shared:
//CHECK:       <*A:59:21, ?>
//CHECK:      induction:
//CHECK:       <K:60:13, 4>:[Int,1,383,1]
//CHECK:      read only:
//CHECK:       <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4>
//CHECK:      lock:
//CHECK:       <K:60:13, 4>
//CHECK:      header access:
//CHECK:       <K:60:13, 4>
//CHECK:      explicit access:
//CHECK:       <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4>
//CHECK:      explicit access (separate):
//CHECK:       <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:      lock (separate):
//CHECK:       <K:60:13, 4>
//CHECK:      direct access (separate):
//CHECK:       <*A:59:21, ?> <A:59:21, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4>
//CHECK:  loop at depth 1 Adi.func.c:70:3
//CHECK:    shared:
//CHECK:     <*A:59:21, ?>
//CHECK:    private:
//CHECK:     <J:60:10, 4> | <K:60:13, 4> | <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:    induction:
//CHECK:     <I:60:7, 4>:[Int,1,383,1]
//CHECK:    reduction:
//CHECK:     <Eps:61:10, 8>:max
//CHECK:    read only:
//CHECK:     <A:59:21, 8>
//CHECK:    lock:
//CHECK:     <I:60:7, 4>
//CHECK:    header access:
//CHECK:     <I:60:7, 4>
//CHECK:    explicit access:
//CHECK:     <A:59:21, 8> | <Eps:61:10, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4> | <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:    explicit access (separate):
//CHECK:     <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
//CHECK:    lock (separate):
//CHECK:     <I:60:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:59:21, ?> <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
//CHECK:   loop at depth 2 Adi.func.c:71:5
//CHECK:     shared:
//CHECK:      <*A:59:21, ?>
//CHECK:     private:
//CHECK:      <K:60:13, 4> | <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:     induction:
//CHECK:      <J:60:10, 4>:[Int,1,383,1]
//CHECK:     reduction:
//CHECK:      <Eps:61:10, 8>:max
//CHECK:     read only:
//CHECK:      <A:59:21, 8> | <I:60:7, 4>
//CHECK:     lock:
//CHECK:      <J:60:10, 4>
//CHECK:     header access:
//CHECK:      <J:60:10, 4>
//CHECK:     explicit access:
//CHECK:      <A:59:21, 8> | <Eps:61:10, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4> | <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:     explicit access (separate):
//CHECK:      <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
//CHECK:     lock (separate):
//CHECK:      <J:60:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:59:21, ?> <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
//CHECK:    loop at depth 3 Adi.func.c:72:7
//CHECK:      private:
//CHECK:       <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:      anti:
//CHECK:       <*A:59:21, ?>:[1,1]
//CHECK:      flow:
//CHECK:       <*A:59:21, ?>:[1,1]
//CHECK:      induction:
//CHECK:       <K:60:13, 4>:[Int,1,383,1]
//CHECK:      reduction:
//CHECK:       <Eps:61:10, 8>:max
//CHECK:      read only:
//CHECK:       <A:59:21, 8> | <I:60:7, 4> | <J:60:10, 4>
//CHECK:      lock:
//CHECK:       <K:60:13, 4>
//CHECK:      header access:
//CHECK:       <K:60:13, 4>
//CHECK:      explicit access:
//CHECK:       <A:59:21, 8> | <Eps:61:10, 8> | <I:60:7, 4> | <J:60:10, 4> | <K:60:13, 4> | <Tmp1:73:16, 8> | <Tmp2:74:16, 8>
//CHECK:      explicit access (separate):
//CHECK:       <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
//CHECK:      lock (separate):
//CHECK:       <K:60:13, 4>
//CHECK:      direct access (separate):
//CHECK:       <*A:59:21, ?> <A:59:21, 8> <Eps:61:10, 8> <I:60:7, 4> <J:60:10, 4> <K:60:13, 4> <Tmp1:73:16, 8> <Tmp2:74:16, 8>
