#include <stdlib.h>

void bar(char *);

void foo() {
  char *P = (char *) malloc (100);
  for (int I = 0; I < 100; ++I)
    P[I] = I;
  bar(P);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 malloc_1.c:7:3
//CHECK:    shared:
//CHECK:     <*P:{6:22|6}, ?>
//CHECK:    first private:
//CHECK:     <*P:{6:22|6}, ?>
//CHECK:    dynamic private:
//CHECK:     <*P:{6:22|6}, ?>
//CHECK:    induction:
//CHECK:     <I:7[7:3], 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <P:6, 8>
//CHECK:    lock:
//CHECK:     <I:7[7:3], 4>
//CHECK:    header access:
//CHECK:     <I:7[7:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:7[7:3], 4> | <P:6, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7[7:3], 4> <P:6, 8>
//CHECK:    lock (separate):
//CHECK:     <I:7[7:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <*P:{6:22|6}, ?> <I:7[7:3], 4> <P:6, 8>
