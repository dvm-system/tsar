int IStart, IEnd, JEnd;

void foo(int N, int JStart, double (*U)[N]) {
  for (int I = IStart; I < IEnd; ++I)
    for (int J = JStart; J < JEnd; ++J)
      U[I][J] = U[I][J] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_8.c:4:3
//CHECK:    shared:
//CHECK:     <*U:3, ?> <IEnd, 4> <JEnd, 4>
//CHECK:    private:
//CHECK:     <J:5[5:5], 4>
//CHECK:    induction:
//CHECK:     <I:4[4:3], 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <JStart:3, 4> | <U:3, 8>
//CHECK:    lock:
//CHECK:     <*U:3, ?> <IEnd, 4> <JEnd, 4> | <I:4[4:3], 4>
//CHECK:    header access:
//CHECK:     <*U:3, ?> <IEnd, 4> <JEnd, 4> | <I:4[4:3], 4>
//CHECK:    explicit access:
//CHECK:     <*U:3, ?> <IEnd, 4> <JEnd, 4> | <I:4[4:3], 4> | <J:5[5:5], 4> | <JStart:3, 4> | <U:3, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4[4:3], 4> <IEnd, 4> <J:5[5:5], 4> <JEnd, 4> <JStart:3, 4> <U:3, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4[4:3], 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:3, ?> <I:4[4:3], 4> <IEnd, 4> <J:5[5:5], 4> <JEnd, 4> <JStart:3, 4> <U:3, 8>
//CHECK:   loop at depth 2 shared_8.c:5:5
//CHECK:     shared:
//CHECK:      <*U:3, ?> <JEnd, 4>
//CHECK:     induction:
//CHECK:      <J:5[5:5], 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:4[4:3], 4> | <U:3, 8>
//CHECK:     lock:
//CHECK:      <*U:3, ?> <JEnd, 4> | <J:5[5:5], 4>
//CHECK:     header access:
//CHECK:      <*U:3, ?> <JEnd, 4> | <J:5[5:5], 4>
//CHECK:     explicit access:
//CHECK:      <*U:3, ?> <JEnd, 4> | <I:4[4:3], 4> | <J:5[5:5], 4> | <U:3, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:4[4:3], 4> <J:5[5:5], 4> <JEnd, 4> <U:3, 8>
//CHECK:     lock (separate):
//CHECK:      <J:5[5:5], 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:3, ?> <I:4[4:3], 4> <J:5[5:5], 4> <JEnd, 4> <U:3, 8>
