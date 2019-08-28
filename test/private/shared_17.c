int IStart, IEnd, JStart, JEnd;

void foo(double (* restrict U)[100]) {
  int I, J;
  for (int I = IStart; I < IEnd; ++I) {
    for (J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;  
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_17.c:5:3
//CHECK:    shared:
//CHECK:     <*U:3:29, ?>
//CHECK:    private:
//CHECK:     <J:4:10, 4>
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4> | <JStart, 4> | <U:3:29, 8>
//CHECK:    lock:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <IEnd, 4> | <J:4:10, 4> | <JEnd, 4> | <JStart, 4> | <U:3:29, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4> <J:4:10, 4> <JEnd, 4> <JStart, 4> <U:3:29, 8>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:3:29, ?> <I:5:12, 4> <IEnd, 4> <J:4:10, 4> <JEnd, 4> <JStart, 4> <U:3:29, 8>
//CHECK:   loop at depth 2 shared_17.c:6:5
//CHECK:     shared:
//CHECK:      <*U:3:29, ?>
//CHECK:     induction:
//CHECK:      <J:4:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:5:12, 4> | <JEnd, 4> | <U:3:29, 8>
//CHECK:     lock:
//CHECK:      <J:4:10, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:4:10, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:5:12, 4> | <J:4:10, 4> | <JEnd, 4> | <U:3:29, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:5:12, 4> <J:4:10, 4> <JEnd, 4> <U:3:29, 8>
//CHECK:     lock (separate):
//CHECK:      <J:4:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:3:29, ?> <I:5:12, 4> <J:4:10, 4> <JEnd, 4> <U:3:29, 8>
