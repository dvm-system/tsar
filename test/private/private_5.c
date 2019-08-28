int foo(int Y) {
  int I, X;
  X = -5;
  for (I = 0; I < 10; ++I) {
    if (Y > 0)
      X = I;
    X = X + 1;
    Y = X;
  }
  return Y;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_5.c:4:3
//CHECK:    output:
//CHECK:     <X:2:10, 4> | <Y:1:13, 4>
//CHECK:    anti:
//CHECK:     <X:2:10, 4> | <Y:1:13, 4>
//CHECK:    flow:
//CHECK:     <X:2:10, 4> | <Y:1:13, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4> | <Y:1:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
