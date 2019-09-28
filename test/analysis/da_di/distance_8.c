const int N = 100;

double A[N][N];

void foo() {
  int I, J;
  for (I = 1; I < N; ++I) {
    for (J = 0; J < N; ++J)
      A[I][J] = A[I][J] + I + J;
    for (J = 1; J < N; ++J)
      A[I][J] = A[I-1][J] + A[I][J-1];
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_8.c:7:3
//CHECK:    private:
//CHECK:     <J:6:10, 4>
//CHECK:    flow:
//CHECK:     <A, 80000>:[1,1]
//CHECK:    induction:
//CHECK:     <I:6:7, 4>:[Int,1,100,1]
//CHECK:    lock:
//CHECK:     <I:6:7, 4>
//CHECK:    header access:
//CHECK:     <I:6:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:6:7, 4> | <J:6:10, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6:7, 4> <J:6:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 80000> <I:6:7, 4> <J:6:10, 4>
//CHECK:   loop at depth 2 distance_8.c:10:5
//CHECK:     flow:
//CHECK:      <A, 80000>:[1,1]
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,1,100,1]
//CHECK:     read only:
//CHECK:      <I:6:7, 4>
//CHECK:     lock:
//CHECK:      <J:6:10, 4>
//CHECK:     header access:
//CHECK:      <J:6:10, 4>
//CHECK:     explicit access:
//CHECK:      <I:6:7, 4> | <J:6:10, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:6:7, 4> <J:6:10, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 80000> <I:6:7, 4> <J:6:10, 4>
//CHECK:   loop at depth 2 distance_8.c:8:5
//CHECK:     shared:
//CHECK:      <A, 80000>
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,0,100,1]
//CHECK:     read only:
//CHECK:      <I:6:7, 4>
//CHECK:     lock:
//CHECK:      <J:6:10, 4>
//CHECK:     header access:
//CHECK:      <J:6:10, 4>
//CHECK:     explicit access:
//CHECK:      <I:6:7, 4> | <J:6:10, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:6:7, 4> <J:6:10, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 80000> <I:6:7, 4> <J:6:10, 4>
