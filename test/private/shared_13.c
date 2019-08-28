int IStart, IEnd, JStart, JEnd;

void bar();

void foo(double (* restrict U)[100]) {
  int I, J;
  for (int I = IStart; I < IEnd; ++I) {
    ++JStart;
    // JStart is not invariant for outer loop.
    // So, analyzer conservatively assumes dependencies for outer loop.
    for (J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;  
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_13.c:7:3
//CHECK:    private:
//CHECK:     <J:6:10, 4>
//CHECK:    shared:
//CHECK:     <*U:5:29, ?>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,,,1] | <JStart, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4> | <U:5:29, 8>
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <IEnd, 4> | <J:6:10, 4> | <JEnd, 4> | <JStart, 4> | <U:5:29, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:29, 8>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:5:29, ?> <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:29, 8>
//CHECK:   loop at depth 2 shared_13.c:11:5
//CHECK:     shared:
//CHECK:      <*U:5:29, ?>
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:7:12, 4> | <JEnd, 4> | <U:5:29, 8>
//CHECK:     lock:
//CHECK:      <J:6:10, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:6:10, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:7:12, 4> | <J:6:10, 4> | <JEnd, 4> | <U:5:29, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:29, 8>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:5:29, ?> <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:29, 8>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 shared_13.c:7:3
//SAFE:    private:
//SAFE:     <J:6:10, 4>
//SAFE:    output:
//SAFE:     <*U:5:29, ?> | <JStart, 4>
//SAFE:    anti:
//SAFE:     <*U:5:29, ?> | <JStart, 4>
//SAFE:    flow:
//SAFE:     <*U:5:29, ?> | <JStart, 4>
//SAFE:    induction:
//SAFE:     <I:7:12, 4>:[Int,,,1]
//SAFE:    read only:
//SAFE:     <IEnd, 4> | <JEnd, 4> | <U:5:29, 8>
//SAFE:    lock:
//SAFE:     <I:7:12, 4> | <IEnd, 4>
//SAFE:    header access:
//SAFE:     <I:7:12, 4> | <IEnd, 4>
//SAFE:    explicit access:
//SAFE:     <I:7:12, 4> | <IEnd, 4> | <J:6:10, 4> | <JEnd, 4> | <JStart, 4> | <U:5:29, 8>
//SAFE:    explicit access (separate):
//SAFE:     <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:29, 8>
//SAFE:    lock (separate):
//SAFE:     <I:7:12, 4> <IEnd, 4>
//SAFE:    direct access (separate):
//SAFE:     <*U:5:29, ?> <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:29, 8>
//SAFE:   loop at depth 2 shared_13.c:11:5
//SAFE:     shared:
//SAFE:      <*U:5:29, ?>
//SAFE:     induction:
//SAFE:      <J:6:10, 4>:[Int,,,1]
//SAFE:     read only:
//SAFE:      <I:7:12, 4> | <JEnd, 4> | <U:5:29, 8>
//SAFE:     lock:
//SAFE:      <J:6:10, 4> | <JEnd, 4>
//SAFE:     header access:
//SAFE:      <J:6:10, 4> | <JEnd, 4>
//SAFE:     explicit access:
//SAFE:      <I:7:12, 4> | <J:6:10, 4> | <JEnd, 4> | <U:5:29, 8>
//SAFE:     explicit access (separate):
//SAFE:      <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:29, 8>
//SAFE:     lock (separate):
//SAFE:      <J:6:10, 4> <JEnd, 4>
//SAFE:     direct access (separate):
//SAFE:      <*U:5:29, ?> <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:29, 8>
