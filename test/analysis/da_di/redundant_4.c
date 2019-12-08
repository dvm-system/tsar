int A[10], B[11];
int * bar();

void foo() {
  for (int I = 0; I < 10; ++I) {
    int *X;
    if (I > 10)
      X = bar();
    A[I] = A[I] + 1;
    B[I+1] = B[I] + 1;
    if (I > 10)
      X[0] = I;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 redundant_4.c:5:3
//CHECK:    private:
//CHECK:     <X:6:10, 8>
//CHECK:    output:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11
//CHECK:    anti:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11
//CHECK:    flow:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44>:[1,1] bar():8:11
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,10,1]
//CHECK:    redundant:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11 | <X:6:10, 8>
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11 | <I:5:12, 4> | <X:6:10, 8>
//CHECK:    address access:
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <X:6:10, 8> bar():8:11
//CHECK:    redundant (separate):
//CHECK:     <*X:{12:7|6:10}, ?> <X:6:10, 8> bar():8:11
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    address access (separate):
//CHECK:     bar():8:11
//CHECK:    direct access (separate):
//CHECK:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> <I:5:12, 4> <X:6:10, 8> bar():8:11
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 redundant_4.c:5:3
//REDUNDANT:    shared:
//REDUNDANT:     <A, 40>
//REDUNDANT:    flow:
//REDUNDANT:     <B, 44>:[1,1]
//REDUNDANT:    induction:
//REDUNDANT:     <I:5:12, 4>:[Int,0,10,1]
//REDUNDANT:    redundant:
//REDUNDANT:     <*X:{12:7|6:10}, ?> <A, 40> <B, 44> bar():8:11 | <X:6:10, 8>
//REDUNDANT:    lock:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <*X:{12:7|6:10}, ?> <X:6:10, 8> bar():8:11
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <A, 40> <B, 44> <I:5:12, 4>
