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
//CHECK:    output:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <I:6:12, 4>
//CHECK:    anti:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <I:6:12, 4>
//CHECK:    flow:
//CHECK:     <A, 40> <B, 44>:[1,1] <Nx, 4> | <I:6:12, 4>
//CHECK:    redundant:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <Nx1:7:9, 4>
//CHECK:    lock:
//CHECK:     <I:6:12, 4>
//CHECK:    header access:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <I:6:12, 4>
//CHECK:    explicit access:
//CHECK:     <A, 40> <B, 44> <Nx, 4> | <I:6:12, 4> | <Nx1:7:9, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <Nx, 4> <Nx1:7:9, 4>
//CHECK:    redundant (separate):
//CHECK:     <Nx, 4> <Nx1:7:9, 4>
