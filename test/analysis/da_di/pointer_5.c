int foo() {
  int X, S = 0;
  for(int I = 0; I < 100; ++I) {
    X = I;
    int *Y = &X;
    int Z = X * 5;
    *Y = Z + *Y;
    S = S + X;
  }
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_5.c:3:3
//CHECK:    private:
//CHECK:     <X:2:7, 4> <Y[0]:{7:15|7:6|5:10}, 4> | <Y:5:10, 8> | <Z:6:9, 4>
//CHECK:    induction:
//CHECK:     <I:3:11, 4>:[Int,0,100,1]
//CHECK:    reduction:
//CHECK:     <S:2:10, 4>:add
//CHECK:    redundant:
//CHECK:     <X:2:7, 4> <Y[0]:{7:15|7:6|5:10}, 4> | <Y:5:10, 8>
//CHECK:    lock:
//CHECK:     <I:3:11, 4>
//CHECK:    header access:
//CHECK:     <I:3:11, 4>
//CHECK:    explicit access:
//CHECK:     <I:3:11, 4> | <S:2:10, 4> | <X:2:7, 4> <Y[0]:{7:15|7:6|5:10}, 4> | <Y:5:10, 8> | <Z:6:9, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:3:11, 4> <S:2:10, 4> <X:2:7, 4> <Y:5:10, 8> <Y[0]:{7:15|7:6|5:10}, 4> <Z:6:9, 4>
//CHECK:    redundant (separate):
//CHECK:     <Y:5:10, 8> <Y[0]:{7:15|7:6|5:10}, 4>
//CHECK:    lock (separate):
//CHECK:     <I:3:11, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:3:11, 4> <S:2:10, 4> <X:2:7, 4> <Y:5:10, 8> <Y[0]:{7:15|7:6|5:10}, 4> <Z:6:9, 4>
