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

void init(int L, double (* restrict A)[L], double (* restrict B)[L]) {
  for (int I = 0; I < L; ++I)
    for (int J = 0; J < L; ++J) {
      A[I][J] = 0;
      if (I == 0 || J == 0 || I == L - 1 || J == L - 1)
        B[I][J] = 0;
      else
        B[I][J] = 3 + I + J;
    }
}

double iter(int L, double (* restrict A)[L], double (* restrict B)[L]) {
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
  return Eps;
}

int main(int Argc, char **Argv) {
  int L = atoi(Argv[1]);
  int ITMAX = atoi(Argv[2]);
  double (*A)[L] = malloc(L * L * sizeof(double));
  double (*B)[L] = malloc(L * L * sizeof(double));
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
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'init':
//CHECK:  loop at depth 1 Jacobi.func.c:17:3
//CHECK:    shared:
//CHECK:     <*A:16, ?> | <*B:16, ?>
//CHECK:    first private:
//CHECK:     <*A:16, ?> | <*B:16, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:16, ?> | <*B:16, ?>
//CHECK:    private:
//CHECK:     <J:18[18:5], 4>
//CHECK:    induction:
//CHECK:     <I:17[17:3], 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:16, 8> | <B:16, 8> | <L:16, 4>
//CHECK:    lock:
//CHECK:     <I:17[17:3], 4> | <L:16, 4>
//CHECK:    header access:
//CHECK:     <I:17[17:3], 4> | <L:16, 4>
//CHECK:    explicit access:
//CHECK:     <A:16, 8> | <B:16, 8> | <I:17[17:3], 4> | <J:18[18:5], 4> | <L:16, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:16, 8> <B:16, 8> <I:17[17:3], 4> <J:18[18:5], 4> <L:16, 4>
//CHECK:    lock (separate):
//CHECK:     <I:17[17:3], 4> <L:16, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:16, ?> <*B:16, ?> <A:16, 8> <B:16, 8> <I:17[17:3], 4> <J:18[18:5], 4> <L:16, 4>
//CHECK:   loop at depth 2 Jacobi.func.c:18:5
//CHECK:     shared:
//CHECK:      <*A:16, ?> | <*B:16, ?>
//CHECK:     first private:
//CHECK:      <*A:16, ?> | <*B:16, ?>
//CHECK:     dynamic private:
//CHECK:      <*A:16, ?> | <*B:16, ?>
//CHECK:     induction:
//CHECK:      <J:18[18:5], 4>:[Int,0,,1]
//CHECK:     read only:
//CHECK:      <A:16, 8> | <B:16, 8> | <I:17[17:3], 4> | <L:16, 4>
//CHECK:     lock:
//CHECK:      <J:18[18:5], 4> | <L:16, 4>
//CHECK:     header access:
//CHECK:      <J:18[18:5], 4> | <L:16, 4>
//CHECK:     explicit access:
//CHECK:      <A:16, 8> | <B:16, 8> | <I:17[17:3], 4> | <J:18[18:5], 4> | <L:16, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:16, 8> <B:16, 8> <I:17[17:3], 4> <J:18[18:5], 4> <L:16, 4>
//CHECK:     lock (separate):
//CHECK:      <J:18[18:5], 4> <L:16, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:16, ?> <*B:16, ?> <A:16, 8> <B:16, 8> <I:17[17:3], 4> <J:18[18:5], 4> <L:16, 4>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'iter':
//CHECK:  loop at depth 1 Jacobi.func.c:29:3
//CHECK:    shared:
//CHECK:     <*A:27, ?>
//CHECK:    private:
//CHECK:     <J:30[30:5], 4> | <Tmp:31[30:37], 8>
//CHECK:    induction:
//CHECK:     <I:29[29:3], 4>:[Int,1,,1]
//CHECK:    reduction:
//CHECK:     <Eps:28, 8>:max
//CHECK:    read only:
//CHECK:     <*B:27, ?> | <A:27, 8> | <B:27, 8> | <L:27, 4>
//CHECK:    lock:
//CHECK:     <I:29[29:3], 4> | <L:27, 4>
//CHECK:    header access:
//CHECK:     <I:29[29:3], 4> | <L:27, 4>
//CHECK:    explicit access:
//CHECK:     <A:27, 8> | <B:27, 8> | <Eps:28, 8> | <I:29[29:3], 4> | <J:30[30:5], 4> | <L:27, 4> | <Tmp:31[30:37], 8>
//CHECK:    explicit access (separate):
//CHECK:     <A:27, 8> <B:27, 8> <Eps:28, 8> <I:29[29:3], 4> <J:30[30:5], 4> <L:27, 4> <Tmp:31[30:37], 8>
//CHECK:    lock (separate):
//CHECK:     <I:29[29:3], 4> <L:27, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:27, ?> <*B:27, ?> <A:27, 8> <B:27, 8> <Eps:28, 8> <I:29[29:3], 4> <J:30[30:5], 4> <L:27, 4> <Tmp:31[30:37], 8>
//CHECK:   loop at depth 2 Jacobi.func.c:30:5
//CHECK:     shared:
//CHECK:      <*A:27, ?>
//CHECK:     private:
//CHECK:      <Tmp:31[30:37], 8>
//CHECK:     induction:
//CHECK:      <J:30[30:5], 4>:[Int,1,,1]
//CHECK:     reduction:
//CHECK:      <Eps:28, 8>:max
//CHECK:     read only:
//CHECK:      <*B:27, ?> | <A:27, 8> | <B:27, 8> | <I:29[29:3], 4> | <L:27, 4>
//CHECK:     lock:
//CHECK:      <J:30[30:5], 4> | <L:27, 4>
//CHECK:     header access:
//CHECK:      <J:30[30:5], 4> | <L:27, 4>
//CHECK:     explicit access:
//CHECK:      <A:27, 8> | <B:27, 8> | <Eps:28, 8> | <I:29[29:3], 4> | <J:30[30:5], 4> | <L:27, 4> | <Tmp:31[30:37], 8>
//CHECK:     explicit access (separate):
//CHECK:      <A:27, 8> <B:27, 8> <Eps:28, 8> <I:29[29:3], 4> <J:30[30:5], 4> <L:27, 4> <Tmp:31[30:37], 8>
//CHECK:     lock (separate):
//CHECK:      <J:30[30:5], 4> <L:27, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:27, ?> <*B:27, ?> <A:27, 8> <B:27, 8> <Eps:28, 8> <I:29[29:3], 4> <J:30[30:5], 4> <L:27, 4> <Tmp:31[30:37], 8>
//CHECK:  loop at depth 1 Jacobi.func.c:35:3
//CHECK:    shared:
//CHECK:     <*B:27, ?>
//CHECK:    first private:
//CHECK:     <*B:27, ?>
//CHECK:    dynamic private:
//CHECK:     <*B:27, ?>
//CHECK:    private:
//CHECK:     <J:36[36:5], 4>
//CHECK:    induction:
//CHECK:     <I:35[35:3], 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <*A:27, ?> | <A:27, 8> | <B:27, 8> | <L:27, 4>
//CHECK:    lock:
//CHECK:     <I:35[35:3], 4> | <L:27, 4>
//CHECK:    header access:
//CHECK:     <I:35[35:3], 4> | <L:27, 4>
//CHECK:    explicit access:
//CHECK:     <A:27, 8> | <B:27, 8> | <I:35[35:3], 4> | <J:36[36:5], 4> | <L:27, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:27, 8> <B:27, 8> <I:35[35:3], 4> <J:36[36:5], 4> <L:27, 4>
//CHECK:    lock (separate):
//CHECK:     <I:35[35:3], 4> <L:27, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:27, ?> <*B:27, ?> <A:27, 8> <B:27, 8> <I:35[35:3], 4> <J:36[36:5], 4> <L:27, 4>
//CHECK:   loop at depth 2 Jacobi.func.c:36:5
//CHECK:     shared:
//CHECK:      <*B:27, ?>
//CHECK:     first private:
//CHECK:      <*B:27, ?>
//CHECK:     dynamic private:
//CHECK:      <*B:27, ?>
//CHECK:     induction:
//CHECK:      <J:36[36:5], 4>:[Int,1,,1]
//CHECK:     read only:
//CHECK:      <*A:27, ?> | <A:27, 8> | <B:27, 8> | <I:35[35:3], 4> | <L:27, 4>
//CHECK:     lock:
//CHECK:      <J:36[36:5], 4> | <L:27, 4>
//CHECK:     header access:
//CHECK:      <J:36[36:5], 4> | <L:27, 4>
//CHECK:     explicit access:
//CHECK:      <A:27, 8> | <B:27, 8> | <I:35[35:3], 4> | <J:36[36:5], 4> | <L:27, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:27, 8> <B:27, 8> <I:35[35:3], 4> <J:36[36:5], 4> <L:27, 4>
//CHECK:     lock (separate):
//CHECK:      <J:36[36:5], 4> <L:27, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:27, ?> <*B:27, ?> <A:27, 8> <B:27, 8> <I:35[35:3], 4> <J:36[36:5], 4> <L:27, 4>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 Jacobi.func.c:47:3
//CHECK:    private:
//CHECK:     <Eps:48[47:39], 8> | <sapfor.var, 4>
//CHECK:    output:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> printf():49:5
//CHECK:    anti:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> printf():49:5
//CHECK:    flow:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> printf():49:5
//CHECK:    induction:
//CHECK:     <It:47[47:3], 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <A:44, 8> | <B:45, 8> | <ITMAX:43, 4> | <L:42, 4>
//CHECK:    redundant:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> iter():48:18 printf():49:5 | <sapfor.var, 4>
//CHECK:    lock:
//CHECK:     <ITMAX:43, 4> | <It:47[47:3], 4>
//CHECK:    header access:
//CHECK:     <ITMAX:43, 4> | <It:47[47:3], 4>
//CHECK:    explicit access:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> printf():49:5 | <A:44, 8> | <B:45, 8> | <Eps:48[47:39], 8> | <ITMAX:43, 4> | <It:47[47:3], 4> | <L:42, 4> | <sapfor.var, 4>
//CHECK:    address access:
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <malloc():44:20,?> <malloc():45:20,?> iter():48:18 printf():49:5
//CHECK:    explicit access (separate):
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <A:44, 8> <B:45, 8> <Eps:48[47:39], 8> <ITMAX:43, 4> <It:47[47:3], 4> <L:42, 4> <sapfor.var, 4> printf():49:5
//CHECK:    redundant (separate):
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <sapfor.var, 4> iter():48:18
//CHECK:    lock (separate):
//CHECK:     <ITMAX:43, 4> <It:47[47:3], 4>
//CHECK:    address access (separate):
//CHECK:     iter():48:18 printf():49:5
//CHECK:    direct access (separate):
//CHECK:     <*A:{46:11|48:26|53:8|44:12}, ?> <*B:{46:14|48:29|54:8|45:12}, ?> <A:44, 8> <B:45, 8> <Eps:48[47:39], 8> <ITMAX:43, 4> <It:47[47:3], 4> <L:42, 4> <sapfor.var, 4> iter():48:18 printf():49:5
//CHECK:    indirect access (separate):
//CHECK:     <malloc():44:20,?> <malloc():45:20,?>
