double A[100];
int X;

void foo(int Flag) {
  for (int I = 0; I < 100; ++I) {
    // LLVM merge two if-stmt and implicitly transform it to
    // if (Flag) { ... } else { ... }
    // This allow us to perform analysis.
    if (Flag)
      X = A[I];
   A[I] = A[I] + 1;
   if (Flag)
     A[I+1] = X;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 dependence_2.c:5:3
//CHECK:    first private:
//CHECK:     <X, 4>
//CHECK:    dynamic private:
//CHECK:     <X, 4>
//CHECK:    output:
//CHECK:     <A, 800>:[1,1]
//CHECK:    flow:
//CHECK:     <A, 800>:[1,1]
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <Flag:4:14, 4>
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <Flag:4:14, 4> | <I:5:12, 4> | <X, 4>
//CHECK:    explicit access (separate):
//CHECK:     <Flag:4:14, 4> <I:5:12, 4> <X, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 800> <Flag:4:14, 4> <I:5:12, 4> <X, 4>
