int main() {
  int I, K, N;
  for (I = 0; I < 10; ++I) {
    K = I;
    N = K;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_8.c:3:3
//CHECK:    private:
//CHECK:     <K:2:10, 4> | <N:2:13, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <K:2:10, 4> | <N:2:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <K:2:10, 4> <N:2:13, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <K:2:10, 4> <N:2:13, 4>
