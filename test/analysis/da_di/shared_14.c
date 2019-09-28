int IStart, IEnd, JStart, JEnd;

void bar();

void foo(double *U) {
  int I, J;
  for (int I = IStart; I < IEnd; ++I) {
    // If U alias JStart then JStart is not invariant for outer loop.
    // So, at this moment analyzer conservatively assumes dependencies
    // for inner loop.
    for (J = JStart; J < JEnd; ++J)
      U[J + I] = U[J + I] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_14.c:7:3
//CHECK:    private:
//CHECK:     <J:6:10, 4>
//CHECK:    output:
//CHECK:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//CHECK:    anti:
//CHECK:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//CHECK:    flow:
//CHECK:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <U:5:18, 8>
//CHECK:    lock:
//CHECK:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4> | <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4> | <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <IEnd, 4> <JEnd, 4> <JStart, 4> | <J:6:10, 4> | <U:5:18, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:18, 8>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:5:18, ?> <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:18, 8>
//CHECK:   loop at depth 2 shared_14.c:11:5
//CHECK:     shared:
//CHECK:      <*U:5:18, ?> <JEnd, 4>
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:7:12, 4> | <U:5:18, 8>
//CHECK:     lock:
//CHECK:      <*U:5:18, ?> <JEnd, 4> | <J:6:10, 4>
//CHECK:     header access:
//CHECK:      <*U:5:18, ?> <JEnd, 4> | <J:6:10, 4>
//CHECK:     explicit access:
//CHECK:      <I:7:12, 4> | <J:6:10, 4> | <JEnd, 4> | <U:5:18, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:18, 8>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:5:18, ?> <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:18, 8>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 shared_14.c:7:3
//SAFE:    private:
//SAFE:     <J:6:10, 4>
//SAFE:    output:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//SAFE:    anti:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//SAFE:    flow:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4>
//SAFE:    induction:
//SAFE:     <I:7:12, 4>:[Int,,,1]
//SAFE:    read only:
//SAFE:     <U:5:18, 8>
//SAFE:    lock:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4> | <I:7:12, 4>
//SAFE:    header access:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4> | <I:7:12, 4>
//SAFE:    explicit access:
//SAFE:     <*U:5:18, ?> <IEnd, 4> <JEnd, 4> <JStart, 4> | <I:7:12, 4> | <J:6:10, 4> | <U:5:18, 8>
//SAFE:    explicit access (separate):
//SAFE:     <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:18, 8>
//SAFE:    lock (separate):
//SAFE:     <I:7:12, 4> <IEnd, 4>
//SAFE:    direct access (separate):
//SAFE:     <*U:5:18, ?> <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:18, 8>
//SAFE:   loop at depth 2 shared_14.c:11:5
//SAFE:     output:
//SAFE:      <*U:5:18, ?> <JEnd, 4>
//SAFE:     anti:
//SAFE:      <*U:5:18, ?> <JEnd, 4>
//SAFE:     flow:
//SAFE:      <*U:5:18, ?> <JEnd, 4>
//SAFE:     induction:
//SAFE:      <J:6:10, 4>:[Int,,,1]
//SAFE:     read only:
//SAFE:      <I:7:12, 4> | <U:5:18, 8>
//SAFE:     lock:
//SAFE:      <*U:5:18, ?> <JEnd, 4> | <J:6:10, 4>
//SAFE:     header access:
//SAFE:      <*U:5:18, ?> <JEnd, 4> | <J:6:10, 4>
//SAFE:     explicit access:
//SAFE:      <*U:5:18, ?> <JEnd, 4> | <I:7:12, 4> | <J:6:10, 4> | <U:5:18, 8>
//SAFE:     explicit access (separate):
//SAFE:      <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:18, 8>
//SAFE:     lock (separate):
//SAFE:      <J:6:10, 4> <JEnd, 4>
//SAFE:     direct access (separate):
//SAFE:      <*U:5:18, ?> <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:18, 8>
