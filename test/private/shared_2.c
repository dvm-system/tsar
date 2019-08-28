double U[100];
int IStart, IEnd;

void foo() {
  for (int I = IStart; I < IEnd; I = I + 5)
    U[I] = U[I] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_2.c:5:3
//CHECK:    shared:
//CHECK:     <U, 800>
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,,,5]
//CHECK:    read only:
//CHECK:     <IEnd, 4>
//CHECK:    lock:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4> <U, 800>
