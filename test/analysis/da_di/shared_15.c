double U[100][100];
int IStart, IEnd, JStart, JEnd;

void foo() {
  int I, J;
  for (int I = IStart; I < IEnd; ++I) {
    ++JStart;
    // JStart is not invariant for outer loop.
    // So, analyzer at this moment conservatively assumes dependencies
    // for outer loop.
    for (J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_15.c:6:3
//CHECK:    private:
//CHECK:     <J:5:10, 4>
//CHECK:    shared:
//CHECK:     <U, 80000>
//CHECK:    output:
//CHECK:     <JStart, 4>
//CHECK:    anti:
//CHECK:     <JStart, 4>
//CHECK:    flow:
//CHECK:     <JStart, 4>
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4>
//CHECK:    lock:
//CHECK:     <I:6:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:6:12, 4> | <IEnd, 4> | <J:5:10, 4> | <JEnd, 4> | <JStart, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:6:12, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4> <U, 80000>
//CHECK:   loop at depth 2 shared_15.c:11:5
//CHECK:     shared:
//CHECK:      <U, 80000>
//CHECK:     induction:
//CHECK:      <J:5:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:6:12, 4> | <JEnd, 4>
//CHECK:     lock:
//CHECK:      <J:5:10, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:5:10, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:6:12, 4> | <J:5:10, 4> | <JEnd, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:6:12, 4> <J:5:10, 4> <JEnd, 4>
//CHECK:     lock (separate):
//CHECK:      <J:5:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:6:12, 4> <J:5:10, 4> <JEnd, 4> <U, 80000>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 shared_15.c:6:3
//SAFE:    private:
//SAFE:     <J:5:10, 4>
//SAFE:    output:
//SAFE:     <JStart, 4> | <U, 80000>
//SAFE:    anti:
//SAFE:     <JStart, 4> | <U, 80000>
//SAFE:    flow:
//SAFE:     <JStart, 4> | <U, 80000>
//SAFE:    induction:
//SAFE:     <I:6:12, 4>:[Int,,,1]
//SAFE:    read only:
//SAFE:     <IEnd, 4> | <JEnd, 4>
//SAFE:    lock:
//SAFE:     <I:6:12, 4> | <IEnd, 4>
//SAFE:    header access:
//SAFE:     <I:6:12, 4> | <IEnd, 4>
//SAFE:    explicit access:
//SAFE:     <I:6:12, 4> | <IEnd, 4> | <J:5:10, 4> | <JEnd, 4> | <JStart, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:6:12, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4>
//SAFE:    lock (separate):
//SAFE:     <I:6:12, 4> <IEnd, 4>
//SAFE:    direct access (separate):
//SAFE:     <I:6:12, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4> <U, 80000>
//SAFE:   loop at depth 2 shared_15.c:11:5
//SAFE:     shared:
//SAFE:      <U, 80000>
//SAFE:     induction:
//SAFE:      <J:5:10, 4>:[Int,,,1]
//SAFE:     read only:
//SAFE:      <I:6:12, 4> | <JEnd, 4>
//SAFE:     lock:
//SAFE:      <J:5:10, 4> | <JEnd, 4>
//SAFE:     header access:
//SAFE:      <J:5:10, 4> | <JEnd, 4>
//SAFE:     explicit access:
//SAFE:      <I:6:12, 4> | <J:5:10, 4> | <JEnd, 4>
//SAFE:     explicit access (separate):
//SAFE:      <I:6:12, 4> <J:5:10, 4> <JEnd, 4>
//SAFE:     lock (separate):
//SAFE:      <J:5:10, 4> <JEnd, 4>
//SAFE:     direct access (separate):
//SAFE:      <I:6:12, 4> <J:5:10, 4> <JEnd, 4> <U, 80000>
