double U[100][100][100][5];
int IStart, IEnd, JStart, JEnd, KStart, KEnd;

void foo() {
  int I, J, K, M;
  for (I = IStart; I < IEnd; ++I)
    for (J = JStart; J < JEnd; ++J)
      for (K = KStart; K < KEnd; ++K)
        for (M = 0; M < 5; ++M)
          U[I][J][K][M] = U[I][J][K][M] + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_11.c:6:3
//CHECK:    shared:
//CHECK:     <U, 40000000>
//CHECK:    private:
//CHECK:     <J:5:10, 4> | <K:5:13, 4> | <M:5:16, 4>
//CHECK:    induction:
//CHECK:     <I:5:7, 4>:[Int,,,1]
//CHECK:    read only:
//CHECK:     <IEnd, 4> | <JEnd, 4> | <JStart, 4> | <KEnd, 4> | <KStart, 4>
//CHECK:    lock:
//CHECK:     <I:5:7, 4> | <IEnd, 4>
//CHECK:    header access:
//CHECK:     <I:5:7, 4> | <IEnd, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:7, 4> | <IEnd, 4> | <J:5:10, 4> | <JEnd, 4> | <JStart, 4> | <K:5:13, 4> | <KEnd, 4> | <KStart, 4> | <M:5:16, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:7, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4> <K:5:13, 4> <KEnd, 4> <KStart, 4> <M:5:16, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:7, 4> <IEnd, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:7, 4> <IEnd, 4> <J:5:10, 4> <JEnd, 4> <JStart, 4> <K:5:13, 4> <KEnd, 4> <KStart, 4> <M:5:16, 4> <U, 40000000>
//CHECK:   loop at depth 2 shared_11.c:7:5
//CHECK:     shared:
//CHECK:      <U, 40000000>
//CHECK:     private:
//CHECK:      <K:5:13, 4> | <M:5:16, 4>
//CHECK:     induction:
//CHECK:      <J:5:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:5:7, 4> | <JEnd, 4> | <KEnd, 4> | <KStart, 4>
//CHECK:     lock:
//CHECK:      <J:5:10, 4> | <JEnd, 4>
//CHECK:     header access:
//CHECK:      <J:5:10, 4> | <JEnd, 4>
//CHECK:     explicit access:
//CHECK:      <I:5:7, 4> | <J:5:10, 4> | <JEnd, 4> | <K:5:13, 4> | <KEnd, 4> | <KStart, 4> | <M:5:16, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:5:7, 4> <J:5:10, 4> <JEnd, 4> <K:5:13, 4> <KEnd, 4> <KStart, 4> <M:5:16, 4>
//CHECK:     lock (separate):
//CHECK:      <J:5:10, 4> <JEnd, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:5:7, 4> <J:5:10, 4> <JEnd, 4> <K:5:13, 4> <KEnd, 4> <KStart, 4> <M:5:16, 4> <U, 40000000>
//CHECK:    loop at depth 3 shared_11.c:8:7
//CHECK:      shared:
//CHECK:       <U, 40000000>
//CHECK:      private:
//CHECK:       <M:5:16, 4>
//CHECK:      induction:
//CHECK:       <K:5:13, 4>:[Int,,,1]
//CHECK:      read only:
//CHECK:       <I:5:7, 4> | <J:5:10, 4> | <KEnd, 4>
//CHECK:      lock:
//CHECK:       <K:5:13, 4> | <KEnd, 4>
//CHECK:      header access:
//CHECK:       <K:5:13, 4> | <KEnd, 4>
//CHECK:      explicit access:
//CHECK:       <I:5:7, 4> | <J:5:10, 4> | <K:5:13, 4> | <KEnd, 4> | <M:5:16, 4>
//CHECK:      explicit access (separate):
//CHECK:       <I:5:7, 4> <J:5:10, 4> <K:5:13, 4> <KEnd, 4> <M:5:16, 4>
//CHECK:      lock (separate):
//CHECK:       <K:5:13, 4> <KEnd, 4>
//CHECK:      direct access (separate):
//CHECK:       <I:5:7, 4> <J:5:10, 4> <K:5:13, 4> <KEnd, 4> <M:5:16, 4> <U, 40000000>
//CHECK:     loop at depth 4 shared_11.c:9:9
//CHECK:       shared:
//CHECK:        <U, 40000000>
//CHECK:       induction:
//CHECK:        <M:5:16, 4>:[Int,0,5,1]
//CHECK:       read only:
//CHECK:        <I:5:7, 4> | <J:5:10, 4> | <K:5:13, 4>
//CHECK:       lock:
//CHECK:        <M:5:16, 4>
//CHECK:       header access:
//CHECK:        <M:5:16, 4>
//CHECK:       explicit access:
//CHECK:        <I:5:7, 4> | <J:5:10, 4> | <K:5:13, 4> | <M:5:16, 4>
//CHECK:       explicit access (separate):
//CHECK:        <I:5:7, 4> <J:5:10, 4> <K:5:13, 4> <M:5:16, 4>
//CHECK:       lock (separate):
//CHECK:        <M:5:16, 4>
//CHECK:       direct access (separate):
//CHECK:        <I:5:7, 4> <J:5:10, 4> <K:5:13, 4> <M:5:16, 4> <U, 40000000>
