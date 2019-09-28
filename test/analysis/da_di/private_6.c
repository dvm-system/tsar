int foo(int Y) {
  int I, X;
  for (I = 0; I < 10; ++I) {
    if (Y > 0)
      X = I;
    if (Y > 0)
      X = X + 1;
  }
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_6.c:3:3
//CHECK:    first private:
//CHECK:     <X:2:10, 4>
//CHECK:    dynamic private:
//CHECK:     <X:2:10, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <Y:1:13, 4>
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
//CHECK    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 private_6.c:3:3
//SAFE:    output:
//SAFE:     <X:2:10, 4>
//SAFE:    anti:
//SAFE:     <X:2:10, 4>
//SAFE:    flow:
//SAFE:     <X:2:10, 4>
//SAFE:    induction:
//SAFE:     <I:2:7, 4>:[Int,0,10,1]
//SAFE:    read only:
//SAFE:     <Y:1:13, 4>
//SAFE:    lock:
//SAFE:     <I:2:7, 4>
//SAFE:    header access:
//SAFE:     <I:2:7, 4>
//SAFE:    explicit access:
//SAFE:     <I:2:7, 4> | <X:2:10, 4> | <Y:1:13, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
//SAFE:    lock (separate):
//SAFE:     <I:2:7, 4>
//SAFE:    direct access (separate):
//SAFE:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
