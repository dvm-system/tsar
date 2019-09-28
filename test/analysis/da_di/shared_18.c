int IStart, IEnd, JStart, JEnd;

void bar();

void foo(double * restrict U) {
  int I, J;
  for (int I = IStart; I < IEnd; ++I) {
    // U is not alias JStart due to 'restrict' keyword.
    // Hence, JStart is invariant for outer loop and
    // analyzer can prove absence of data dependence in the inner loop.
    for (J = JStart; J < JEnd; ++J)
      U[J + I] = U[J + I] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_18.c:7:3
//CHECK:    private:
//CHECK:     <J:6:10, 4>
//CHECK:    output:
//CHECK:     <*U:5:28, ?>
//CHECK:    anti:
//CHECK:     <*U:5:28, ?>
//CHECK:    flow:
//CHECK:     <*U:5:28, ?>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4> | <JStart, 4> | <U:5:28, 8>
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <IEnd, 4> | <J:6:10, 4> | <JEnd, 4> | <JStart, 4> | <U:5:28, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:28, 8>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <*U:5:28, ?> <I:7:12, 4> <IEnd, 4> <J:6:10, 4> <JEnd, 4> <JStart, 4> <U:5:28, 8>
//CHECK:   loop at depth 2 shared_18.c:11:5
//CHECK:     shared:
//CHECK:      <*U:5:28, ?>
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:7:12, 4> | <JEnd, 4> | <U:5:28, 8>
//CHECK:     lock:
//CHECK:      <J:6:10, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:6:10, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:7:12, 4> | <J:6:10, 4> | <JEnd, 4> | <U:5:28, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:28, 8>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <*U:5:28, ?> <I:7:12, 4> <J:6:10, 4> <JEnd, 4> <U:5:28, 8>
