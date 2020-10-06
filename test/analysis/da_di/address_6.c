void bar(int *X) { *X = 10; }

void baz();

int foo() {
  int X, S = 1;
  bar(&X);
  for (int I = 0; I < 10; ++I) {
    X = I;
    S = S * X;
  }
  baz();
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 address_6.c:8:3
//CHECK:    private:
//CHECK:     <X:6:7, 4>
//CHECK:    induction:
//CHECK:     <I:8[8:3], 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:6, 4>:mult
//CHECK:    no promoted scalar:
//CHECK:     <X:6:7, 4>
//CHECK:    lock:
//CHECK:     <I:8[8:3], 4> | <X:6:7, 4>
//CHECK:    header access:
//CHECK:     <I:8[8:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:8[8:3], 4> | <S:6, 4> | <X:6:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:8[8:3], 4> <S:6, 4> <X:6:7, 4>
//CHECK:    lock (separate):
//CHECK:     <I:8[8:3], 4> <X:6:7, 4>
//CHECK:    no promoted scalar (separate):
//CHECK:     <X:6:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:8[8:3], 4> <S:6, 4> <X:6:7, 4>
