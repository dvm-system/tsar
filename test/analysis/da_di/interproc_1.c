int foo(int * restrict X) {
   return *X + 1;
}

void bar(int X, int N, float * restrict A) {
  int I;
  for (int I = 0; I < N; ++I)
    A[I] = foo(&X);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 interproc_1.c:7:3
//CHECK:    shared:
//CHECK:     <*A:5:41, ?>
//CHECK:    first private:
//CHECK:     <*A:5:41, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:5:41, ?>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:5:41, 8> | <N:5:21, 4> | <X:5:14, ?>
//CHECK:    no promoted scalar:
//CHECK:     <X:5:14, ?>
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <N:5:21, 4> | <X:5:14, ?>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <N:5:21, 4>
//CHECK:    explicit access:
//CHECK:     <A:5:41, 8> | <I:7:12, 4> | <N:5:21, 4> | <X:5:14, ?>
//CHECK:    address access:
//CHECK:     <X:5:14, ?>
//CHECK:    explicit access (separate):
//CHECK:     <A:5:41, 8> <I:7:12, 4> <N:5:21, 4> <X:5:14, ?>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <N:5:21, 4> <X:5:14, ?>
//CHECK:    address access (separate):
//CHECK:     <X:5:14, ?>
//CHECK:    no promoted scalar (separate):
//CHECK:     <X:5:14, ?>
//CHECK:    direct access (separate):
//CHECK:     <*A:5:41, ?> <A:5:41, 8> <I:7:12, 4> <N:5:21, 4> <X:5:14, ?>
