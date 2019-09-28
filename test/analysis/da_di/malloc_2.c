#include <stdlib.h>

void bar(int *);

void foo() {
  int *P = (int *) malloc (100);
  for (int I = 0; I < 100; ++I)
    P[I] = I;
  bar(P);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 malloc_2.c:7:3
//CHECK:    shared:
//CHECK:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//CHECK:    first private:
//CHECK:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//CHECK:    dynamic private:
//CHECK:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <P:6:8, 8>
//CHECK:    redundant:
//CHECK:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//CHECK:    lock:
//CHECK:     <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <P:6:8, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <P:6:8, 8>
//CHECK:    redundant (separate):
//CHECK:     <*P:{8:5|9:7|6:8}, ?>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*P:{8:5|9:7|6:8}, ?> <I:7:12, 4> <P:6:8, 8>
//CHECK:    indirect access (separate):
//CHECK:     <malloc():6:20,?>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 malloc_2.c:7:3
//REDUNDANT:    shared:
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//REDUNDANT:    first private:
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//REDUNDANT:    dynamic private:
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//REDUNDANT:    induction:
//REDUNDANT:     <I:7:12, 4>:[Int,0,100,1]
//REDUNDANT:    read only:
//REDUNDANT:     <P:6:8, 8>
//REDUNDANT:    redundant:
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?> <malloc():6:20,?>
//REDUNDANT:    lock:
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:7:12, 4> | <P:6:8, 8>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:7:12, 4> <P:6:8, 8>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <*P:{8:5|9:7|6:8}, ?> <I:7:12, 4> <P:6:8, 8>
