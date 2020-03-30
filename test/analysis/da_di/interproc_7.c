#include <math.h>

void foo(int *S) {
  for (int I = 0; I < 100; ++I)
    S[I] = sqrt(I);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 interproc_7.c:4:3
//CHECK:    shared:
//CHECK:     <*S:3:15, ?>
//CHECK:    first private:
//CHECK:     <*S:3:15, ?>
//CHECK:    dynamic private:
//CHECK:     <*S:3:15, ?>
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <S:3:15, 8>
//CHECK:    lock:
//CHECK:     <I:4:12, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:4:12, 4> | <S:3:15, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <S:3:15, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*S:3:15, ?> <I:4:12, 4> <S:3:15, 8>
