int A[10], B[11];
void bar();

void foo() {
  for (int I = 0; I < 10; ++I) {
    if (I > 10)
      bar();
    A[I] = A[I] + 1;
    B[I+1] = B[I] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 redundant_2.c:5:3
//CHECK:    output:
//CHECK:     <A, 40> <B, 44> bar():7:7
//CHECK:    anti:
//CHECK:     <A, 40> <B, 44> bar():7:7
//CHECK:    flow:
//CHECK:     <A, 40> <B, 44>:[1,1] bar():7:7
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,10,1]
//CHECK:    redundant:
//CHECK:     <A, 40> <B, 44> bar():7:7
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <A, 40> <B, 44> bar():7:7 | <I:5:12, 4>
//CHECK:    address access:
//CHECK:     <A, 40> <B, 44> bar():7:7
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> bar():7:7
//CHECK:    redundant (separate):
//CHECK:     bar():7:7
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    address access (separate):
//CHECK:     bar():7:7
//CHECK:    direct access (separate):
//CHECK:     <A, 40> <B, 44> <I:5:12, 4> bar():7:7
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 redundant_2.c:5:3
//REDUNDANT:    shared:
//REDUNDANT:     <A, 40>
//REDUNDANT:    flow:
//REDUNDANT:     <B, 44>:[1,1]
//REDUNDANT:    induction:
//REDUNDANT:     <I:5:12, 4>:[Int,0,10,1]
//REDUNDANT:    redundant:
//REDUNDANT:     <A, 40> <B, 44> bar():7:7
//REDUNDANT:    lock:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     bar():7:7
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:5:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <A, 40> <B, 44> <I:5:12, 4>
