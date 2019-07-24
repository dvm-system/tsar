double foo(int JStart) {
  double U[100];
  int I, J;
  JStart = 1;
  for (int I = 0; I < 100; I = I + 10)
    for (J = JStart; J < 10; ++J)
      U[I + J] = U[I + J - 1] + 1;
  return U[50];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_5.c:5:3
//CHECK:    shared:
//CHECK:     <U:2:10, 800>
//CHECK:    private:
//CHECK:     <J:3:10, 4>
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,100,10]
//CHECK:    read only:
//CHECK:     <JStart:1:16, 4>
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <J:3:10, 4> | <JStart:1:16, 4>
//CHECK:   loop at depth 2 distance_5.c:6:5
//CHECK:     flow:
//CHECK:      <U:2:10, 800>:[1,1]
//CHECK:     induction:
//CHECK:      <J:3:10, 4>:[Int,1,10,1]
//CHECK:     read only:
//CHECK:      <I:5:12, 4>
//CHECK:     lock:
//CHECK:      <J:3:10, 4>
//CHECK:     header access:
//CHECK:      <J:3:10, 4> | <U:2:10, 800>
//CHECK:     explicit access:
//CHECK:      <I:5:12, 4> | <J:3:10, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 distance_5.c:5:3
//SAFE:    shared:
//SAFE:     <U:2:10, 800>
//SAFE:    private:
//SAFE:     <J:3:10, 4>
//SAFE:    induction:
//SAFE:     <I:5:12, 4>:[Int,0,100,10]
//SAFE:    read only:
//SAFE:     <JStart:1:16, 4>
//SAFE:    lock:
//SAFE:     <I:5:12, 4>
//SAFE:    header access:
//SAFE:     <I:5:12, 4>
//SAFE:    explicit access:
//SAFE:     <I:5:12, 4> | <J:3:10, 4> | <JStart:1:16, 4>
//SAFE:   loop at depth 2 distance_5.c:6:5
//SAFE:     flow:
//SAFE:      <U:2:10, 800>
//SAFE:     induction:
//SAFE:      <J:3:10, 4>:[Int,1,10,1]
//SAFE:     read only:
//SAFE:      <I:5:12, 4>
//SAFE:     lock:
//SAFE:      <J:3:10, 4>
//SAFE:     header access:
//SAFE:      <J:3:10, 4> | <U:2:10, 800>
//SAFE:     explicit access:
//SAFE:      <I:5:12, 4> | <J:3:10, 4>