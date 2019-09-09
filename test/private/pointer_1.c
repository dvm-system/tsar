int X, Y, N;

void foo(double * restrict A) {
  int *P;
  X = 5; Y = 6;
  for (int I = 1; I < N - 1; ++I) {
    P = &X;
    Y = 5;
    P = &Y;
    A[I] = 1 + *P;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_1.c:6:3
//CHECK:    shared:
//CHECK:     <*A:3:28, ?>
//CHECK:    first private:
//CHECK:     <*A:3:28, ?> | <Y, 4>
//CHEKC:    dynamic private:
//CHECK:     <*A:3:28, ?>
//CHECK:    second to last private:
//CHECK:     <Y, 4>
//CHECK:    private:
//CHECK:     <P:4:8, 8>
//CHECK:    output:
//CHECK:     <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    anti:
//CHECK:     <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    flow:
//CHECK:     <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <A:3:28, 8> | <N, 4>
//CHECK:    redundant:
//CHECK:     <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4> | <X, 4>
//CHECK:    lock:
//CHECK:     <I:6:12, 4> | <N, 4> | <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4> | <N, 4>
//CHECK:    explicit access:
//CHECK:     <A:3:28, 8> | <I:6:12, 4> | <N, 4> | <P:4:8, 8> | <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    address access:
//CHECK:      <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <P[0]:{10:17|4:8}, 4> <Y, 4>
//CHECK:    redundant (separate):
//CHECK:     <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4> <N, 4>
//CHECK:    address access (separate)
//CHECK:     <X, 4> <Y, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:3:28, ?> <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 pointer_1.c:6:3
//SAFE:    private:
//SAFE:     <P:4:8, 8>
//SAFE:    output:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE:    anti:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE:    flow:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE:    induction:
//SAFE:     <I:6:12, 4>:[Int,1,,1]
//SAFE:    read only:
//SAFE:     <A:3:28, 8>
//SAFE:    redundant:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE:    lock:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4> | <I:6:12, 4>
//SAFE:    header access:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4> | <I:6:12, 4>
//SAFE:    explicit access:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4> | <A:3:28, 8> | <I:6:12, 4> | <P:4:8, 8>
//SAFE:    address access:
//SAFE:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//SAFE:    explicit access (separate):
//SAFE:     <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <P[0]:{10:17|4:8}, 4> <Y, 4>
//SAFE:    redundant (separate):
//SAFE:     <P[0]:{10:17|4:8}, 4> <X, 4>
//SAFE:    lock (separate):
//SAFE:     <I:6:12, 4> <N, 4>
//SAFE:    address access (separate):
//SAFE:     <X, 4> <Y, 4>
//SAFE:    direct access (separate):
//SAFE:     <*A:3:28, ?> <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 pointer_1.c:6:3
//REDUNDANT:    shared:
//REDUNDANT:     <*A:3:28, ?>
//REDUNDANT:    first private:
//REDUNDANT:     <*A:3:28, ?> | <Y, 4>
//REDUNDANT:    dynamic private:
//REDUNDANT:     <*A:3:28, ?>
//REDUNDANT:    second to last private:
//REDUNDANT:     <Y, 4>
//REDUNDANT:    private:
//REDUNDANT:     <P:4:8, 8>
//REDUNDANT:    induction:
//REDUNDANT:     <I:6:12, 4>:[Int,1,,1]
//REDUNDANT:    read only:
//REDUNDANT:     <A:3:28, 8> | <N, 4>
//REDUNDANT:    redundant:
//REDUNDANT:     <*A:3:28, ?> <N, 4> <P[0]:{10:17|4:8}, 4> <X, 4> <Y, 4>
//REDUNDANT:    lock:
//REDUNDANT:     <I:6:12, 4> | <N, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:6:12, 4> | <N, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <A:3:28, 8> | <I:6:12, 4> | <N, 4> | <P:4:8, 8> | <Y, 4>
//REDUNDANT:    address access:
//REDUNDANT:     <Y, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <Y, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <P[0]:{10:17|4:8}, 4> <X, 4>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:6:12, 4> <N, 4>
//REDUNDANT:    address access (separate):
//REDUNDANT:     <Y, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <*A:3:28, ?> <A:3:28, 8> <I:6:12, 4> <N, 4> <P:4:8, 8> <Y, 4>
