int N;

void bar(int *X);

void foo(double * restrict A) {
  int X, Y, *P;
  X = 5; Y = 6;
  for (int I = 1; I < N - 1; ++I) {
    P = &X;
    Y = 5;
    P = &Y;
    A[I] = X + *P;
  }
  bar(&X);
  bar(&Y);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_2.c:8:3
//CHECK:    shared:
//CHECK:     <*A:5:28, ?>
//CHECK:    first private:
//CHECK:     <*A:5:28, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:5:28, ?>
//CHECK:    private:
//CHECK:     <P:6:14, 8>
//CHECK:    output:
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    anti:
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    flow:
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    induction:
//CHECK:     <I:8:12, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <A:5:28, 8> | <N, 4>
//CHECK:    no promoted scalar:
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    lock:
//CHECK:     <I:8:12, 4> | <N, 4>
//CHECK:    header access:
//CHECK:     <I:8:12, 4> | <N, 4>
//CHECK:    explicit access:
//CHECK:     <A:5:28, 8> | <I:8:12, 4> | <N, 4> | <P:6:14, 8> | <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    address access:
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    explicit access (separate):
//CHECK:     <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4>
//CHECK:    lock (separate):
//CHECK:     <I:8:12, 4>  <N, 4>
//CHECK:    address access (separate):
//CHECK:     <X:6:7, ?> <Y:6:10, ?>
//CHECK:    no promoted scalar (separate):
//CHECK:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//CHECK:    direct access (separate):
//CHECK:     <*A:5:28, ?> <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 pointer_2.c:8:3
//SAFE:    private:
//SAFE:     <P:6:14, 8>
//SAFE:    output:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    anti:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    flow:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    induction:
//SAFE:     <I:8:12, 4>:[Int,1,,1]
//SAFE:    read only:
//SAFE:     <A:5:28, 8>
//SAFE:    no promoted scalar:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    lock:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <I:8:12, 4>
//SAFE:    header access:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <I:8:12, 4>
//SAFE:    explicit access:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <A:5:28, 8> | <I:8:12, 4> | <P:6:14, 8>
//SAFE:    address access:
//SAFE:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    explicit access (separate):
//SAFE:     <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4>
//SAFE:    lock (separate):
//SAFE:     <I:8:12, 4> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    address access (separate):
//SAFE:     <X:6:7, ?> <Y:6:10, ?>
//SAFE:    no promoted scalar (separate):
//SAFE:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//SAFE:    direct access (separate):
//SAFE:     <*A:5:28, ?> <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//UNSAFE_TFM:  loop at depth 1 pointer_2.c:8:3
//UNSAFE_TFM:    private:
//UNSAFE_TFM:     <P:6:14, 8>
//UNSAFE_TFM:    output:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    anti:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    flow:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    induction:
//UNSAFE_TFM:     <I:8:12, 4>:[Int,1,,1]
//UNSAFE_TFM:    read only:
//UNSAFE_TFM:     <A:5:28, 8>
//UNSAFE_TFM:    no promoted scalar:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    redundant:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    lock:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <I:8:12, 4>
//UNSAFE_TFM:    header access:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <I:8:12, 4>
//UNSAFE_TFM:    explicit access:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?> | <A:5:28, 8> | <I:8:12, 4> | <P:6:14, 8>
//UNSAFE_TFM:    address access:
//UNSAFE_TFM:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    explicit access (separate):
//UNSAFE_TFM:     <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4>
//UNSAFE_TFM:    redundant (separate):
//UNSAFE_TFM:     <P[0]:{12:17|6:14}, 4>
//UNSAFE_TFM:    lock (separate):
//UNSAFE_TFM:     <I:8:12, 4> <N, 4>
//UNSAFE_TFM:    address access (separate):
//UNSAFE_TFM:     <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    no promoted scalar (separate):
//UNSAFE_TFM:     <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM:    direct access (separate):
//UNSAFE_TFM:     <*A:5:28, ?> <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//UNSAFE_TFM_and_REDUNDANT:  loop at depth 1 pointer_2.c:8:3
//UNSAFE_TFM_and_REDUNDANT:    shared:
//UNSAFE_TFM_and_REDUNDANT:     <*A:5:28, ?>
//UNSAFE_TFM_and_REDUNDANT:    first private:
//UNSAFE_TFM_and_REDUNDANT:     <*A:5:28, ?> | <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    dynamic private:
//UNSAFE_TFM_and_REDUNDANT:     <*A:5:28, ?>
//UNSAFE_TFM_and_REDUNDANT:    second to last private:
//UNSAFE_TFM_and_REDUNDANT:     <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    private:
//UNSAFE_TFM_and_REDUNDANT:     <P:6:14, 8>
//UNSAFE_TFM_and_REDUNDANT:    induction:
//UNSAFE_TFM_and_REDUNDANT:     <I:8:12, 4>:[Int,1,,1]
//UNSAFE_TFM_and_REDUNDANT:    read only:
//UNSAFE_TFM_and_REDUNDANT:     <A:5:28, 8> | <N, 4> | <X:6:7, ?>
//UNSAFE_TFM_and_REDUNDANT:    no promoted scalar:
//UNSAFE_TFM_and_REDUNDANT:     <X:6:7, ?> | <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    redundant:
//UNSAFE_TFM_and_REDUNDANT:     <*A:5:28, ?> <N, 4> <P[0]:{12:17|6:14}, 4> <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    lock:
//UNSAFE_TFM_and_REDUNDANT:     <I:8:12, 4> | <N, 4>
//UNSAFE_TFM_and_REDUNDANT:    header access:
//UNSAFE_TFM_and_REDUNDANT:     <I:8:12, 4> | <N, 4>
//UNSAFE_TFM_and_REDUNDANT:    explicit access:
//UNSAFE_TFM_and_REDUNDANT:     <A:5:28, 8> | <I:8:12, 4> | <N, 4> | <P:6:14, 8>
//UNSAFE_TFM_and_REDUNDANT:    address access:
//UNSAFE_TFM_and_REDUNDANT:     <X:6:7, ?> | <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    explicit access (separate):
//UNSAFE_TFM_and_REDUNDANT:     <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8>
//UNSAFE_TFM_and_REDUNDANT:    redundant (separate):
//UNSAFE_TFM_and_REDUNDANT:     <P[0]:{12:17|6:14}, 4>
//UNSAFE_TFM_and_REDUNDANT:    lock (separate):
//UNSAFE_TFM_and_REDUNDANT:     <I:8:12, 4> <N, 4>
//UNSAFE_TFM_and_REDUNDANT:    address access (separate):
//UNSAFE_TFM_and_REDUNDANT:     <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    no promoted scalar (separate):
//UNSAFE_TFM_and_REDUNDANT:     <X:6:7, ?> <Y:6:10, ?>
//UNSAFE_TFM_and_REDUNDANT:    direct access (separate):
//UNSAFE_TFM_and_REDUNDANT:     <*A:5:28, ?> <A:5:28, 8> <I:8:12, 4> <N, 4> <P:6:14, 8> <X:6:7, ?> <Y:6:10, ?>
