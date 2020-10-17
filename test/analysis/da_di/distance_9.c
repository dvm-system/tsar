const int N = 100;

double A[N][N][N];

void foo() {
  int I, J, K;
  for (I = 1; I < N; ++I)
    for (J = 0; J < N; ++J)
      for (K = 0; K < N; ++K)
        A[I][J][K] = A[I][J][K] + A[I-1][J][K];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_9.c:7:3
//CHECK:    private:
//CHECK:     <J:6, 4> | <K:6, 4>
//CHECK:    flow:
//CHECK:     <A, 8000000>:[1:1,0:0,0:0]
//CHECK:    induction:
//CHECK:     <I:6, 4>:[Int,1,100,1]
//CHECK:    lock:
//CHECK:     <I:6, 4>
//CHECK:    header access:
//CHECK:     <I:6, 4>
//CHECK:    explicit access:
//CHECK:     <I:6, 4> | <J:6, 4> | <K:6, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6, 4> <J:6, 4> <K:6, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 8000000> <I:6, 4> <J:6, 4> <K:6, 4>
//CHECK:   loop at depth 2 distance_9.c:8:5
//CHECK:     shared:
//CHECK:      <A, 8000000>
//CHECK:     private:
//CHECK:      <K:6, 4>
//CHECK:     induction:
//CHECK:      <J:6, 4>:[Int,0,100,1]
//CHECK:     read only:
//CHECK:      <I:6, 4>
//CHECK:     lock:
//CHECK:      <J:6, 4>
//CHECK:     header access:
//CHECK:      <J:6, 4>
//CHECK:     explicit access:
//CHECK:      <I:6, 4> | <J:6, 4> | <K:6, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:6, 4> <J:6, 4> <K:6, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6, 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 8000000> <I:6, 4> <J:6, 4> <K:6, 4>
//CHECK:    loop at depth 3 distance_9.c:9:7
//CHECK:      shared:
//CHECK:       <A, 8000000>
//CHECK:      induction:
//CHECK:       <K:6, 4>:[Int,0,100,1]
//CHECK:      read only:
//CHECK:       <I:6, 4> | <J:6, 4>
//CHECK:      lock:
//CHECK:       <K:6, 4>
//CHECK:      header access:
//CHECK:       <K:6, 4>
//CHECK:      explicit access:
//CHECK:       <I:6, 4> | <J:6, 4> | <K:6, 4>
//CHECK:      explicit access (separate):
//CHECK:       <I:6, 4> <J:6, 4> <K:6, 4>
//CHECK:      lock (separate):
//CHECK:       <K:6, 4>
//CHECK:      direct access (separate):
//CHECK:       <A, 8000000> <I:6, 4> <J:6, 4> <K:6, 4>
