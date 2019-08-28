int IStart, IEnd, JEnd;

void foo(int N, int JStart, double (*U)[N]) {
  for (int I = IStart; I < IEnd; ++I)
    for (int J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_8.c:4:3
//CHECK:    shared:
//CHECK:     <*U:3:38, ?> <IEnd, 4> <JEnd, 4>
//CHECK:    private:
//CHECK:     <J:5:14, 4>
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <JStart:3:21, 4> | <U:3:38, 8>
//CHECK:    lock:
//CHECK:     <*U:3:38, ?> <IEnd, 4> <JEnd, 4> | <I:4:12, 4>
//CHECK:    header access:
//CHECK:     <*U:3:38, ?> <IEnd, 4> <JEnd, 4> | <I:4:12, 4>
//CHECK:    explicit access:
//CHECK:     <*U:3:38, ?> <IEnd, 4> <JEnd, 4> | <I:4:12, 4> | <J:5:14, 4> | <JStart:3:21, 4> | <U:3:38, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <IEnd, 4> <J:5:14, 4> <JEnd, 4> <JStart:3:21, 4> <U:3:38, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:3:38, ?> <I:4:12, 4> <IEnd, 4> <J:5:14, 4> <JEnd, 4> <JStart:3:21, 4> <U:3:38, 8>
//CHECK:   loop at depth 2 shared_8.c:5:5
//CHECK:     shared:
//CHECK:      <*U:3:38, ?> <JEnd, 4>
//CHECK:     induction:
//CHECK:      <J:5:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:4:12, 4> | <U:3:38, 8>
//CHECK:     lock:
//CHECK:      <*U:3:38, ?> <JEnd, 4> | <J:5:14, 4>
//CHECK:     header access:
//CHECK:      <*U:3:38, ?> <JEnd, 4> | <J:5:14, 4>
//CHECK:     explicit access:
//CHECK:      <*U:3:38, ?> <JEnd, 4> | <I:4:12, 4> | <J:5:14, 4> | <U:3:38, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:4:12, 4> <J:5:14, 4> <JEnd, 4> <U:3:38, 8>
//CHECK:     lock (separate):
//CHECK:      <J:5:14, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:3:38, ?> <I:4:12, 4> <J:5:14, 4> <JEnd, 4> <U:3:38, 8>
