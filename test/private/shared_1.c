double U[100][100];

void foo(int I, int N) {
  for (int J = 0; J < 10; ++J)
    U[I][J] = U[I][J] + 1;  
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_1.c:4:3
//CHECK:    shared:
//CHECK:     <U, 80000>
//CHECK:    induction:
//CHECK:     <J:4:12, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <I:3:14, 4>
//CHECK:    lock:
//CHECK:     <J:4:12, 4>
//CHECK:    header access:
//CHECK:     <J:4:12, 4> | <U, 80000>
//CHECK:    explicit access:
//CHECK:     <I:3:14, 4> | <J:4:12, 4>
