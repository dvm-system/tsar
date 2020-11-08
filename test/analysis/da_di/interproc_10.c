enum { NX = 100, NY = 100 };

double A[NX][NY][5];

void bar(double A[5]) {
  A[0] = 0;
  A[1] = 1;
  A[2] = 2;
  A[3] = 3;
  A[4] = 4;
}

void foo() {
  for (int I = 0; I < NX; ++I) {
    for (int J = 0; J < NY; ++J)
      bar(A[I][J]);
    for (int J = 0; J < NY - 1; ++J)
      for (int M = 0; M < 5; ++M)
        A[I][J][M] = A[I][J][M] + A[I][J + 1][M];
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 interproc_10.c:14:3
//CHECK:    shared:
//CHECK:     <A, 400000>
//CHECK:    private:
//CHECK:     <J:15[15:5], 4> | <J:17[17:5], 4> | <M:18[18:7], 4>
//CHECK:    induction:
//CHECK:     <I:14[14:3], 4>:[Int,0,100,1]
//CHECK:    lock:
//CHECK:     <I:14[14:3], 4>
//CHECK:    header access:
//CHECK:     <I:14[14:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:14[14:3], 4> | <J:15[15:5], 4> | <J:17[17:5], 4> | <M:18[18:7], 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:14[14:3], 4> <J:15[15:5], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:    lock (separate):
//CHECK:     <I:14[14:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 400000> <I:14[14:3], 4> <J:15[15:5], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:   loop at depth 2 interproc_10.c:17:5
//CHECK:     private:
//CHECK:      <M:18[18:7], 4>
//CHECK:     anti:
//CHECK:      <A, 400000>:[1:1,0:0]
//CHECK:     induction:
//CHECK:      <J:17[17:5], 4>:[Int,0,99,1]
//CHECK:     read only:
//CHECK:      <I:14[14:3], 4>
//CHECK:     lock:
//CHECK:      <J:17[17:5], 4>
//CHECK:     header access:
//CHECK:      <J:17[17:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:14[14:3], 4> | <J:17[17:5], 4> | <M:18[18:7], 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:14[14:3], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:     lock (separate):
//CHECK:      <J:17[17:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 400000> <I:14[14:3], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:    loop at depth 3 interproc_10.c:18:7
//CHECK:      shared:
//CHECK:       <A, 400000>
//CHECK:      induction:
//CHECK:       <M:18[18:7], 4>:[Int,0,5,1]
//CHECK:      read only:
//CHECK:       <I:14[14:3], 4> | <J:17[17:5], 4>
//CHECK:      lock:
//CHECK:       <M:18[18:7], 4>
//CHECK:      header access:
//CHECK:       <M:18[18:7], 4>
//CHECK:      explicit access:
//CHECK:       <I:14[14:3], 4> | <J:17[17:5], 4> | <M:18[18:7], 4>
//CHECK:      explicit access (separate):
//CHECK:       <I:14[14:3], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:      lock (separate):
//CHECK:       <M:18[18:7], 4>
//CHECK:      direct access (separate):
//CHECK:       <A, 400000> <I:14[14:3], 4> <J:17[17:5], 4> <M:18[18:7], 4>
//CHECK:   loop at depth 2 interproc_10.c:15:5
//CHECK:     shared:
//CHECK:      <A, 400000>
//CHECK:     first private:
//CHECK:      <A, 400000>
//CHECK:     dynamic private:
//CHECK:      <A, 400000>
//CHECK:     induction:
//CHECK:      <J:15[15:5], 4>:[Int,0,100,1]
//CHECK:     read only:
//CHECK:      <I:14[14:3], 4>
//CHECK:     lock:
//CHECK:      <J:15[15:5], 4>
//CHECK:     header access:
//CHECK:      <J:15[15:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:14[14:3], 4> | <J:15[15:5], 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:14[14:3], 4> <J:15[15:5], 4>
//CHECK:     lock (separate):
//CHECK:      <J:15[15:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 400000> <I:14[14:3], 4> <J:15[15:5], 4>
