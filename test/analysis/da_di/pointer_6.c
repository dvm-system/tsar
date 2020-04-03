int Z;

int foo() {
  int X, S;
  int *SPtr = &S;
  *SPtr = 1;
  for(int I = 0; I < 100; ++I) {
    int *XPtr = &X;
    *XPtr = I;
    S = S * X * Z;
  }
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_6.c:7:3
//CHECK:    private:
//CHECK:     <XPtr:8:10, 8>
//CHECK:    output:
//CHECK:     <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4>
//CHECK:    anti:
//CHECK:     <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4>
//CHECK:    flow:
//CHECK:     <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4>
//CHECK:    induction:
//CHECK:     <I:7:11, 4>:[Int,0,100,1]
//CHECK:    redundant:
//CHECK:     <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4> | <XPtr:8:10, 8>
//CHECK:    lock:
//CHECK:     <I:7:11, 4>
//CHECK:    header access:
//CHECK:     <I:7:11, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:11, 4> | <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4> | <XPtr:8:10, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:11, 4> <S:4:10, 4> <X:4:7, 4> <XPtr:8:10, 8> <XPtr[0]:{9:6|8:10}, 4> <Z, 4>
//CHECK:    redundant (separate):
//CHECK:     <XPtr:8:10, 8> <XPtr[0]:{9:6|8:10}, 4>
//CHECK:    lock (separate):
//CHECK:     <I:7:11, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:7:11, 4> <S:4:10, 4> <X:4:7, 4> <XPtr:8:10, 8> <XPtr[0]:{9:6|8:10}, 4> <Z, 4>
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT:  loop at depth 1 pointer_6.c:7:3
//REDUNDANT:    private:
//REDUNDANT:     <X:4:7, 4>
//REDUNDANT:    induction:
//REDUNDANT:     <I:7:11, 4>:[Int,0,100,1]
//REDUNDANT:    reduction:
//REDUNDANT:     <S:4:10, 4>:mult
//REDUNDANT:    read only:
//REDUNDANT:     <Z, 4>
//REDUNDANT:    redundant:
//REDUNDANT:     <S:4:10, 4> <X:4:7, 4> <XPtr[0]:{9:6|8:10}, 4> <Z, 4> | <XPtr:8:10, 8>
//REDUNDANT:    lock:
//REDUNDANT:     <I:7:11, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:7:11, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:7:11, 4> | <S:4:10, 4> | <X:4:7, 4> | <Z, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:7:11, 4> <S:4:10, 4> <X:4:7, 4> <Z, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     <XPtr:8:10, 8> <XPtr[0]:{9:6|8:10}, 4>
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:7:11, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <I:7:11, 4> <S:4:10, 4> <X:4:7, 4> <Z, 4>
