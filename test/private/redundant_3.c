int A[10], B[11];
int * bar();

void foo() {
  for (int I = 0; I < 10; ++I) {
    if (I > 10)
      bar()[4] = 6;
    A[I] = A[I] + 1;
    B[I+1] = B[I] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 redundant_3.c:5:3
//CHECK:    output:
//CHECK:     <A, 40> <B, 44> <bar():7:7,?> bar():7:7
//CHECK:    anti:
//CHECK:     <A, 40> <B, 44> <bar():7:7,?> bar():7:7
//CHECK:    flow:
//CHECK:     <A, 40> <B, 44>:[1,1] <bar():7:7,?> bar():7:7
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,10,1]
//CHECK:    redundant:
//CHECK:     <A, 40> <B, 44> <bar():7:7,?> bar():7:7
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <A, 40> <B, 44> <bar():7:7,?> bar():7:7 | <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <A, 40> <B, 44> <bar():7:7,?> bar():7:7 | <I:5:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> bar():7:7
//CHECK:    redundant (separate):
//CHECK:     <bar():7:7,?> bar():7:7
