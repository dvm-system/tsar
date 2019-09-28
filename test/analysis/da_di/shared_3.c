int IStart, IEnd;

void foo(double *U) {
  for (int I = IStart; I < IEnd; I = I + 5)
    U[I] = U[I] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_3.c:4:3
//CHECK:    shared:
//CHECK:     <*U:3:18, ?> <IEnd, 4>
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,,,5]
//CHECK:    read only:
//CHECK:     <U:3:18, 8>
//CHECK:    lock:
//CHECK:     <*U:3:18, ?> <IEnd, 4> | <I:4:12, 4>
//CHECK:    header access:
//CHECK:     <*U:3:18, ?> <IEnd, 4> | <I:4:12, 4>
//CHECK:    explicit access:
//CHECK:     <*U:3:18, ?> <IEnd, 4> | <I:4:12, 4> | <U:3:18, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <IEnd, 4> <U:3:18, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:3:18, ?> <I:4:12, 4> <IEnd, 4> <U:3:18, 8>
