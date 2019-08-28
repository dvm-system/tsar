int foo(int I) {
  int X = I;
  while (I < 10) {
    X = I;
    ++I;
  };
  X = X + 1;
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_12.c:3:3
//CHECK:    first private:
//CHECK:     <X:2:7, 4>
//CHECK:    second to last private:
//CHECK:     <X:2:7, 4>
//CHECK:    induction:
//CHECK:     <I:1:13, 4>:[Int,,,1]
//CHECK:    lock:
//CHECK:     <I:1:13, 4>
//CHECK:    header access:
//CHECK:     <I:1:13, 4>
//CHECK:    explicit access:
//CHECK:     <I:1:13, 4> | <X:2:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:1:13, 4> <X:2:7, 4>
//CHECK:    lock (separate):
//CHECK:     <I:1:13, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:1:13, 4> <X:2:7, 4>
