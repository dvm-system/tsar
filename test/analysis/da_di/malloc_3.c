#include <stdlib.h>

void bar(char *);

void foo() {
  char *P = (char *) malloc (100);
  for (int I = 0; I < 100; ++I)
    P[I] = I;
  bar(P);
  P = (char *) malloc (10);
  for (int I = 0; I < 10; ++I)
    P[I] = I;
  bar(P);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 malloc_3.c:7:3
//CHECK:    shared:
//CHECK:     <*P:{6:22|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    first private:
//CHECK:     <*P:{6:22|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    dynamic private:
//CHECK:     <*P:{6:22|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <P:6:9, 8>
//CHECK:    redundant:
//CHECK:     <*P:{6:22|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    lock:
//CHECK:     <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <P:6:9, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <P:6:9, 8>
//CHECK:    redundant (separate):
//CHECK:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?> <I:7:12, 4> <P:6:9, 8>
//CHECK:    indirect access (separate):
//CHECK:     <*P:{6:22|6:9}, ?>
//CHECK:  loop at depth 1 malloc_3.c:11:3
//CHECK:    shared:
//CHECK:     <*P:{10:16|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    first private:
//CHECK:     <*P:{10:16|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    dynamic private:
//CHECK:     <*P:{10:16|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    induction:
//CHECK:     <I:11:12, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <P:6:9, 8>
//CHECK:    redundant:
//CHECK:     <*P:{10:16|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    lock:
//CHECK:     <I:11:12, 4>
//CHECK:    header access:
//CHECK:     <I:11:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:11:12, 4> | <P:6:9, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:11:12, 4> <P:6:9, 8>
//CHECK:    redundant (separate):
//CHECK:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//CHECK:    lock (separate):
//CHECK:     <I:11:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?> <I:11:12, 4> <P:6:9, 8>
//CHECK:    indirect access (separate):
//CHECK:     <*P:{10:16|6:9}, ?>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 malloc_3.c:7:3
//REDUNDANT:    shared:
//REDUNDANT:     <*P:{6:22|6:9}, ?>
//REDUNDANT:    first private:
//REDUNDANT:     <*P:{6:22|6:9}, ?>
//REDUNDANT:    dynamic private:
//REDUNDANT:     <*P:{6:22|6:9}, ?>
//REDUNDANT:    induction:
//REDUNDANT:     <I:7:12, 4>:[Int,0,100,1]
//REDUNDANT:    read only:
//REDUNDANT:     <P:6:9, 8>
//REDUNDANT:    redundant:
//REDUNDANT:     <*P:{6:22|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//REDUNDANT:    lock:
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:7:12, 4> | <P:6:9, 8>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:7:12, 4> <P:6:9, 8>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:7:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <I:7:12, 4> <P:6:9, 8>
//REDUNDANT:  loop at depth 1 malloc_3.c:11:3
//REDUNDANT:    shared:
//REDUNDANT:     <*P:{10:16|6:9}, ?>
//REDUNDANT:    first private:
//REDUNDANT:     <*P:{10:16|6:9}, ?>
//REDUNDANT:    dynamic private:
//REDUNDANT:     <*P:{10:16|6:9}, ?>
//REDUNDANT:    induction:
//REDUNDANT:     <I:11:12, 4>:[Int,0,10,1]
//REDUNDANT:    read only:
//REDUNDANT:     <P:6:9, 8>
//REDUNDANT:    redundant:
//REDUNDANT:     <*P:{10:16|6:9}, ?> <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//REDUNDANT:    lock:
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:11:12, 4> | <P:6:9, 8>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:11:12, 4> <P:6:9, 8>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <*P:{8:5|9:7|12:5|13:7|6:9}, ?>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <I:11:12, 4> <P:6:9, 8>
