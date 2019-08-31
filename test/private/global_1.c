double A[10][10];

void bar(double (*A)[10]);

void foo() {
  bar(A);
  for (int I = 0; I < 10; ++I)
    for (int J = 0; J < 10; ++J)
      A[I][J] = A[I-1][J] * 2;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 global_1.c:7:3
//CHECK:    private:
//CHECK:     <J:8:14, 4>
//CHECK:    flow:
//CHECK:     <A, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <J:8:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <J:8:14, 4>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, ?> <I:7:12, 4> <J:8:14, 4>
//CHECK:   loop at depth 2 global_1.c:8:5
//CHECK:     shared:
//CHECK:      <A, ?>
//CHECK:     induction:
//CHECK:      <J:8:14, 4>:[Int,0,10,1]
//CHECK:     read only:
//CHECK:      <I:7:12, 4>
//CHECK:     lock:
//CHECK:      <J:8:14, 4>
//CHECK:     header access:
//CHECK:      <J:8:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:7:12, 4> | <J:8:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:7:12, 4> <J:8:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:8:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <A, ?> <I:7:12, 4> <J:8:14, 4>
