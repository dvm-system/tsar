struct S {
  int *Y;
};

int foo(int I, struct S *restrict X) {
   return *X->Y = I;
}

void bar(int N, struct S *restrict X, float * restrict A) {
  int I;
  for (int I = 0; I < N; ++I)
    // X is a complex type with a pointer sub-type. So, we assume that
    // this call may have a side effect.
    A[I] = foo(I, X);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 interproc_3.c:11:3
//CHECK:    output:
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> foo():14:12
//CHECK:    anti:
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> foo():14:12
//CHECK:    flow:
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> foo():14:12
//CHECK:    induction:
//CHECK:     <I:11:12, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:9:56, 8> | <N:9:14, 4> | <X:9:36, 8>
//CHECK:    lock:
//CHECK:     <I:11:12, 4> | <N:9:14, 4>
//CHECK:    header access:
//CHECK:     <I:11:12, 4> | <N:9:14, 4>
//CHECK:    explicit access:
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> foo():14:12 | <A:9:56, 8> | <I:11:12, 4> | <N:9:14, 4> | <X:9:36, 8>
//CHECK:    explicit access (separate):
//CHECK:     <*X:9:36, ?> <A:9:56, 8> <I:11:12, 4> <N:9:14, 4> <X:9:36, 8> foo():14:12
//CHECK:    lock (separate):
//CHECK:     <I:11:12, 4> <N:9:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:9:56, ?> <*X:9:36, ?> <A:9:56, 8> <I:11:12, 4> <N:9:14, 4> <X:9:36, 8> foo():14:12
