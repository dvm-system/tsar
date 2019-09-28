double U[100][100];
int IStart, IEnd, JEnd;

void foo(int JStart) {
  for (int I = IStart; I < IEnd; ++I)
    for (int J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_7.c:5:3
//CHECK:    shared:
//CHECK:     <U, 80000>
//CHECK:    private:
//CHECK:     <J:6:14, 4>
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4> | <JStart:4:14, 4>
//CHECK:    lock:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <IEnd, 4> | <J:6:14, 4> | <JEnd, 4> | <JStart:4:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4> <J:6:14, 4> <JEnd, 4> <JStart:4:14, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4> <J:6:14, 4> <JEnd, 4> <JStart:4:14, 4> <U, 80000>
//CHECK:   loop at depth 2 shared_7.c:6:5
//CHECK:     shared:
//CHECK:      <U, 80000>
//CHECK:     induction:
//CHECK:      <J:6:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:5:12, 4> | <JEnd, 4>
//CHECK:     lock:
//CHECK:      <J:6:14, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:6:14, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:5:12, 4> | <J:6:14, 4> | <JEnd, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:5:12, 4> <J:6:14, 4> <JEnd, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6:14, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:5:12, 4> <J:6:14, 4> <JEnd, 4> <U, 80000>
