int bar(int I) { return I + 1; }

int foo() {
  int S = 0;
  for (int I = 0; I < 10; ++I)
    S += bar(I);
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_call_1.c:5:3
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:4:7, 4>:add
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <S:4:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <S:4:7, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <S:4:7, 4>
