int IStart, IEnd;

void foo(double * restrict U, int IStep) {
  for (int I = IStart; I < IEnd; I = I + IStep)
    U[I] = U[I] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_5.c:4:3
//CHECK:    shared:
//CHECK:     <*U:3, ?>
//CHECK:    induction:
//CHECK:     <I:4[4:3], 4>:[Int,,,]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <IStep:3, 4> | <U:3, 8>
//CHECK:    lock:
//CHECK:     <I:4[4:3], 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:4[4:3], 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:4[4:3], 4> | <IEnd, 4> | <IStep:3, 4> | <U:3, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4[4:3], 4> <IEnd, 4> <IStep:3, 4> <U:3, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4[4:3], 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:3, ?> <I:4[4:3], 4> <IEnd, 4> <IStep:3, 4> <U:3, 8>
