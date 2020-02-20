struct S {
  int Y;
};

int foo(int I, struct S *restrict X) {
   return X->Y = I;
}

void bar(int N, struct S *restrict X, float * restrict A) {
  int I;
  for (int I = 0; I < N; ++I)
    A[I] = foo(I, X);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 interproc_4.c:11:3
//CHECK:    shared:
//CHECK:     <*A:9:56, ?>
//CHECK:    first private:
//CHECK:     <*A:9:56, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:9:56, ?>
//CHECK:    private:
//CHECK:     <*X:9:36, ?>
//CHECK:    induction:
//CHECK:     <I:11:12, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:9:56, 8> | <N:9:14, 4> | <X:9:36, 8>
//CHECK:    lock:
//CHECK:     <I:11:12, 4> | <N:9:14, 4>
//CHECK:    header access:
//CHECK:     <I:11:12, 4> | <N:9:14, 4>
//CHECK:    explicit access:
//CHECK:     <*X:9:36, ?> | <A:9:56, 8> | <I:11:12, 4> | <N:9:14, 4> | <X:9:36, 8>
//CHECK:    explicit access (separate):
//CHECK:     <*X:9:36, ?> <A:9:56, 8> <I:11:12, 4> <N:9:14, 4> <X:9:36, 8>
//CHECK:    lock (separate):
//CHECK:     <I:11:12, 4> <N:9:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> <A:9:56, 8> <I:11:12, 4> <N:9:14, 4> <X:9:36, 8>
