int X;
long long bar() { return (long long)&X; }

long long foo() {
  long long S = 0;
  for (int I = 0; I < 10; ++I)
    S += bar();
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_call_2.c:6:3
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:5:13, 8>:add
//CHECK:    lock:
//CHECK:     <I:6:12, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:6:12, 4> | <S:5:13, 8>
//CHECK:    address access:
//CHECK:     bar():7:10
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <S:5:13, 8>
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4>
//CHECK:    address access (separate):
//CHECK:     bar():7:10
//CHECK:    direct access (separate):
//CHECK:     <I:6:12, 4> <S:5:13, 8> bar():7:10
