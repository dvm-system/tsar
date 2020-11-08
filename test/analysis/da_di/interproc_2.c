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
//CHECK:     <*A:5, ?> <X[0]:5, 4> foo():8:12
//CHECK:    anti:
//CHECK:     <*A:5, ?> <X[0]:5, 4> foo():8:12
//CHECK:    flow:
//CHECK:     <*A:5, ?> <X[0]:5, 4> foo():8:12
//CHECK:    induction:
//CHECK:     <I:7[7:3], 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:5, 8> | <N:5, 4> | <X:5, 8>
//CHECK:    redundant:
//CHECK:     <*A:5, ?> <X[0]:5, 4> foo():8:12
//CHECK:    lock:
//CHECK:     <I:7[7:3], 4> | <N:5, 4>
//CHECK:    header access:
//CHECK:     <I:7[7:3], 4> | <N:5, 4>
//CHECK:    explicit access:
//CHECK:     <*A:5, ?> <X[0]:5, 4> foo():8:12 | <A:5, 8> | <I:7[7:3], 4> | <N:5, 4> | <X:5, 8>
//CHECK:    explicit access (separate):
//CHECK:     <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:5, 8> <X[0]:5, 4> foo():8:12
//CHECK:    redundant (separate):
//CHECK:     foo():8:12
//CHECK:    lock (separate):
//CHECK:     <I:7[7:3], 4> <N:5, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:5, ?> <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:5, 8> foo():8:12
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//REDUNDANT:  loop at depth 1 interproc_2.c:7:3
//REDUNDANT:    shared:
//REDUNDANT:     <*A:5, ?>
//REDUNDANT:    first private:
//REDUNDANT:     <*A:5, ?> | <X[0]:5, 4>
//REDUNDANT:    dynamic private:
//REDUNDANT:     <*A:5, ?>
//REDUNDANT:    second to last private:
//REDUNDANT:     <X[0]:5, 4>
//REDUNDANT:    induction:
//REDUNDANT:     <I:7[7:3], 4>:[Int,0,,1]
//REDUNDANT:    read only:
//REDUNDANT:     <A:5, 8> | <N:5, 4> | <X:5, 8>
//REDUNDANT:    redundant:
//REDUNDANT:     <*A:5, ?> <X[0]:5, 4> foo():8:12
//REDUNDANT:    lock:
//REDUNDANT:     <I:7[7:3], 4> | <N:5, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:7[7:3], 4> | <N:5, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <A:5, 8> | <I:7[7:3], 4> | <N:5, 4> | <X:5, 8> | <X[0]:5, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:5, 8> <X[0]:5, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     foo():8:12
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:7[7:3], 4> <N:5, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <*A:5, ?> <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:5, 8>
