int foo(int Y) {
  int I, X;
  for (I = 0; I < 10; ++I)
    if (Y > 0)
      X = I;
  return 0;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_3.c:3:3
//CHECK:    private:
//CHECK:     <X:2:10, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <Y:1:13, 4>
//CHECK:    redundant:
//CHECK:     <X:2:10, 4>
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4> | <Y:1:13, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
//CHECK:    redundant (separate):
//CHECK:     <X:2:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4> <Y:1:13, 4>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 private_3.c:3:3
//REDUNDANT:    induction:
//REDUNDANT:     <I:2:7, 4>:[Int,0,10,1]
//REDUNDANT:    read only:
//REDUNDANT:     <Y:1:13, 4>
//REDUNDANT:    redundant:
//REDUNDANT:     <X:2:10, 4>
//REDUNDANT:    lock:
//REDUNDANT:     <I:2:7, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:2:7, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:2:7, 4> | <Y:1:13, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:2:7, 4> <Y:1:13, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <X:2:10, 4>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:2:7, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <I:2:7, 4> <Y:1:13, 4>
