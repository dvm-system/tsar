int foo(int I, long long X) {
   return *(int *)X = I;
}

void bar(int N, int *X, float * restrict A) {
  int I;
  for (int I = 0; I < N; ++I)
    A[I] = foo(I, (long long)X);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 interproc_2.c:7:3
//CHECK:    output:
//CHECK:     <*A:5:42, ?> foo():8:12
//CHECK:    anti:
//CHECK:     <*A:5:42, ?> foo():8:12
//CHECK:    flow:
//CHECK:     <*A:5:42, ?> foo():8:12
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:5:42, 8> | <N:5:14, 4> | <X:5:22, 8>
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <N:5:14, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <N:5:14, 4>
//CHECK:    explicit access:
//CHECK:     <*A:5:42, ?> foo():8:12 | <A:5:42, 8> | <I:7:12, 4> | <N:5:14, 4> | <X:5:22, 8>
//CHECK:    explicit access (separate):
//CHECK:     <A:5:42, 8> <I:7:12, 4> <N:5:14, 4> <X:5:22, 8> foo():8:12
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <N:5:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:5:42, ?> <A:5:42, 8> <I:7:12, 4> <N:5:14, 4> <X:5:22, 8> foo():8:12
