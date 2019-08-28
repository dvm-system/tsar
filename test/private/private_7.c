int main() {
  int I, X, Y;
  X = 0;
  for (I = 0; I < 10; ++I) {
    if (I == 0)
      X = X + 1;
  }
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_7.c:4:3
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <X:2:10, 4>:add
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
