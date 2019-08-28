int Nx;
int A[10], B[11];
void bar();

void foo() {
  for (int I = 0; I < 10; ++I) {
    int Nx1 = Nx + 1;
    A[I] = A[I] + 1;
    B[I+1] = B[I] + 1;
  }
  bar();
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 redundant_1.c:6:3
//CHECK:    private:
//CHECK:     <Nx1:7:9, 4>
//CHECK:    flow:
//CHECK:     <A, 40> <B, 44>:[1,1] <Nx, 4>
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,0,10,1]
//CHECK:    redundant:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <Nx1:7:9, 4>
//CHECK:    lock:
//CHECK:     <I:6:12, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4>
//CHECK:    explicit access:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <I:6:12, 4> | <Nx1:7:9, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <Nx, 4> <Nx1:7:9, 4>
//CHECK:    redundant (separate):
//CHECK:     <Nx, 4> <Nx1:7:9, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 40> <B, 44> <I:6:12, 4> <Nx, 4> <Nx1:7:9, 4>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 redundant_1.c:6:3
//REDUNDANT:    shared:
//REDUNDANT:     <A, 40>
//REDUNDANT:    flow:
//REDUNDANT:     <B, 44>:[1,1]
//REDUNDANT:    induction:
//REDUNDANT:     <I:6:12, 4>:[Int,0,10,1]
//REDUNDANT:    redundant:
//REDUNDANT:     <A, 40> <B, 44> <Nx, 4> | <Nx1:7:9, 4>
//REDUNDANT:    lock:
//REDUNDANT:     <I:6:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:6:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:6:12, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:6:12, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <Nx, 4> <Nx1:7:9, 4>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:6:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <A, 40> <B, 44> <I:6:12, 4>
