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
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 Jacobi.c:21:3
//CHECK:    shared:
//CHECK:     <A, 512> | <B, 512>
//CHECK:    first private:
//CHECK:     <A, 512> | <B, 512>
//CHECK:    dynamic private:
//CHECK:     <A, 512> | <B, 512>
//CHECK:    private:
//CHECK:     <J:22[22:5], 4> | <sapfor.var, 4>
//CHECK:    induction:
//CHECK:     <I:21[21:3], 4>:[Int,0,8,1]
//CHECK:    redundant:
//CHECK:     <sapfor.var, 4>
//CHECK:    lock:
//CHECK:     <I:21[21:3], 4>
//CHECK:    header access:
//CHECK:     <I:21[21:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:21[21:3], 4> | <J:22[22:5], 4> | <sapfor.var, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:21[21:3], 4> <J:22[22:5], 4> <sapfor.var, 4>
//CHECK:    redundant (separate):
//CHECK:     <sapfor.var, 4>
//CHECK:    lock (separate):
//CHECK:     <I:21[21:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 512> <B, 512> <I:21[21:3], 4> <J:22[22:5], 4> <sapfor.var, 4>
//CHECK:   loop at depth 2 Jacobi.c:22:5
//CHECK:     shared:
//CHECK:      <A, 512> | <B, 512>
//CHECK:     first private:
//CHECK:      <A, 512> | <B, 512>
//CHECK:     dynamic private:
//CHECK:      <A, 512> | <B, 512>
//CHECK:     induction:
//CHECK:      <J:22[22:5], 4>:[Int,0,8,1]
//CHECK:     read only:
//CHECK:      <I:21[21:3], 4>
//CHECK:     lock:
//CHECK:      <J:22[22:5], 4>
//CHECK:     header access:
//CHECK:      <J:22[22:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:21[21:3], 4> | <J:22[22:5], 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:21[21:3], 4> <J:22[22:5], 4>
//CHECK:     lock (separate):
//CHECK:      <J:22[22:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 512> <B, 512> <I:21[21:3], 4> <J:22[22:5], 4>
//CHECK:  loop at depth 1 Jacobi.c:29:3
//CHECK:    private:
//CHECK:     <Eps:30[29:39], 8> | <I:31[31:5], 4> | <I:37[37:5], 4> | <J:32[32:7], 4> | <J:38[38:7], 4> | <Tmp:33[32:39], 8> | <sapfor.var, 4>
//CHECK:    output:
//CHECK:     <A, 512> <B, 512> printf():40:5
//CHECK:    anti:
//CHECK:     <A, 512> <B, 512> printf():40:5
//CHECK:    flow:
//CHECK:     <A, 512> <B, 512> printf():40:5
//CHECK:    induction:
//CHECK:     <It:29[29:3], 4>:[Int,1,,1]
//CHECK:    redundant:
//CHECK:     <sapfor.var, 4>
//CHECK:    lock:
//CHECK:     <It:29[29:3], 4>
//CHECK:    header access:
//CHECK:     <It:29[29:3], 4>
//CHECK:    explicit access:
//CHECK:     <A, 512> <B, 512> printf():40:5 | <Eps:30[29:39], 8> | <I:31[31:5], 4> | <I:37[37:5], 4> | <It:29[29:3], 4> | <J:32[32:7], 4> | <J:38[38:7], 4> | <Tmp:33[32:39], 8> | <sapfor.var, 4>
//CHECK:    address access:
//CHECK:     <A, 512> <B, 512> printf():40:5
//CHECK:    explicit access (separate):
//CHECK:     <Eps:30[29:39], 8> <I:31[31:5], 4> <I:37[37:5], 4> <It:29[29:3], 4> <J:32[32:7], 4> <J:38[38:7], 4> <Tmp:33[32:39], 8> <sapfor.var, 4> printf():40:5
//CHECK:    redundant (separate):
//CHECK:     <sapfor.var, 4>
//CHECK:    lock (separate):
//CHECK:     <It:29[29:3], 4>
//CHECK:    address access (separate):
//CHECK:     printf():40:5
//CHECK:    direct access (separate):
//CHECK:     <A, 512> <B, 512> <Eps:30[29:39], 8> <I:31[31:5], 4> <I:37[37:5], 4> <It:29[29:3], 4> <J:32[32:7], 4> <J:38[38:7], 4> <Tmp:33[32:39], 8> <sapfor.var, 4> printf():40:5
//CHECK:   loop at depth 2 Jacobi.c:37:5
//CHECK:     shared:
//CHECK:      <B, 512>
//CHECK:     first private:
//CHECK:      <B, 512>
//CHECK:     dynamic private:
//CHECK:      <B, 512>
//CHECK:     private:
//CHECK:      <J:38[38:7], 4> | <sapfor.var, 4>
//CHECK:     induction:
//CHECK:      <I:37[37:5], 4>:[Int,1,7,1]
//CHECK:     read only:
//CHECK:      <A, 512>
//CHECK:     redundant:
//CHECK:      <sapfor.var, 4>
//CHECK:     lock:
//CHECK:      <I:37[37:5], 4>
//CHECK:     header access:
//CHECK:      <I:37[37:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:37[37:5], 4> | <J:38[38:7], 4> | <sapfor.var, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:37[37:5], 4> <J:38[38:7], 4> <sapfor.var, 4>
//CHECK:     redundant (separate):
//CHECK:      <sapfor.var, 4>
//CHECK:     lock (separate):
//CHECK:      <I:37[37:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 512> <B, 512> <I:37[37:5], 4> <J:38[38:7], 4> <sapfor.var, 4>
//CHECK:    loop at depth 3 Jacobi.c:38:7
//CHECK:      shared:
//CHECK:       <B, 512>
//CHECK:      first private:
//CHECK:       <B, 512>
//CHECK:      dynamic private:
//CHECK:       <B, 512>
//CHECK:      induction:
//CHECK:       <J:38[38:7], 4>:[Int,1,7,1]
//CHECK:      read only:
//CHECK:       <A, 512> | <I:37[37:5], 4>
//CHECK:      lock:
//CHECK:       <J:38[38:7], 4>
//CHECK:      header access:
//CHECK:       <J:38[38:7], 4>
//CHECK:      explicit access:
//CHECK:       <I:37[37:5], 4> | <J:38[38:7], 4>
//CHECK:      explicit access (separate):
//CHECK:       <I:37[37:5], 4> <J:38[38:7], 4>
//CHECK:      lock (separate):
//CHECK:       <J:38[38:7], 4>
//CHECK:      direct access (separate):
//CHECK:       <A, 512> <B, 512> <I:37[37:5], 4> <J:38[38:7], 4>
//CHECK:   loop at depth 2 Jacobi.c:31:5
//CHECK:     shared:
//CHECK:      <A, 512>
//CHECK:     private:
//CHECK:      <J:32[32:7], 4> | <Tmp:33[32:39], 8> | <sapfor.var, 4>
//CHECK:     induction:
//CHECK:      <I:31[31:5], 4>:[Int,1,7,1]
//CHECK:     reduction:
//CHECK:      <Eps:30[29:39], 8>:max
//CHECK:     read only:
//CHECK:      <B, 512>
//CHECK:     redundant:
//CHECK:      <sapfor.var, 4>
//CHECK:     lock:
//CHECK:      <I:31[31:5], 4>
//CHECK:     header access:
//CHECK:      <I:31[31:5], 4>
//CHECK:     explicit access:
//CHECK:      <Eps:30[29:39], 8> | <I:31[31:5], 4> | <J:32[32:7], 4> | <Tmp:33[32:39], 8> | <sapfor.var, 4>
//CHECK:     explicit access (separate):
//CHECK:      <Eps:30[29:39], 8> <I:31[31:5], 4> <J:32[32:7], 4> <Tmp:33[32:39], 8> <sapfor.var, 4>
//CHECK:     redundant (separate):
//CHECK:      <sapfor.var, 4>
//CHECK:     lock (separate):
//CHECK:      <I:31[31:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 512> <B, 512> <Eps:30[29:39], 8> <I:31[31:5], 4> <J:32[32:7], 4> <Tmp:33[32:39], 8> <sapfor.var, 4>
//CHECK:    loop at depth 3 Jacobi.c:32:7
//CHECK:      shared:
//CHECK:       <A, 512>
//CHECK:      private:
//CHECK:       <Tmp:33[32:39], 8>
//CHECK:      induction:
//CHECK:       <J:32[32:7], 4>:[Int,1,7,1]
//CHECK:      reduction:
//CHECK:       <Eps:30[29:39], 8>:max
//CHECK:      read only:
//CHECK:       <B, 512> | <I:31[31:5], 4>
//CHECK:      lock:
//CHECK:       <J:32[32:7], 4>
//CHECK:      header access:
//CHECK:       <J:32[32:7], 4>
//CHECK:      explicit access:
//CHECK:       <Eps:30[29:39], 8> | <I:31[31:5], 4> | <J:32[32:7], 4> | <Tmp:33[32:39], 8>
//CHECK:      explicit access (separate):
//CHECK:       <Eps:30[29:39], 8> <I:31[31:5], 4> <J:32[32:7], 4> <Tmp:33[32:39], 8>
//CHECK:      lock (separate):
//CHECK:       <J:32[32:7], 4>
//CHECK:      direct access (separate):
//CHECK:       <A, 512> <B, 512> <Eps:30[29:39], 8> <I:31[31:5], 4> <J:32[32:7], 4> <Tmp:33[32:39], 8>
