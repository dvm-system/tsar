void bar(double *X, double Y);

void foo (int N) {
  double X;
  for (int I = 0; I < 10; ++I)
    bar(&X, X);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 address_1.c:5:3
//CHECK:    output:
//CHECK:     <X:4:10, ?> bar():6:5
//CHECK:    anti:
//CHECK:     <X:4:10, ?> bar():6:5
//CHECK:    flow:
//CHECK:     <X:4:10, ?> bar():6:5
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <X:4:10, ?> bar():6:5 | <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <X:4:10, ?> bar():6:5 | <I:5:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <X:4:10, ?> bar():6:5
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 address_1.c:5:3
//SAFE:    output:
//SAFE:     <?,?> <X:4:10, ?> bar():6:5
//SAFE:    anti:
//SAFE:     <?,?> <X:4:10, ?> bar():6:5
//SAFE:    flow:
//SAFE:     <?,?> <X:4:10, ?> bar():6:5
//SAFE:    induction:
//SAFE:     <I:5:12, 4>:[Int,0,10,1]
//SAFE:    lock:
//SAFE:     <I:5:12, 4>
//SAFE:    header access:
//SAFE:     <?,?> <X:4:10, ?> bar():6:5 | <I:5:12, 4>
//SAFE:    explicit access:
//SAFE:     <?,?> <X:4:10, ?> bar():6:5 | <I:5:12, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:5:12, 4> <X:4:10, ?> bar():6:5
//SAFE:    lock (separate):
//SAFE:     <I:5:12, 4>
