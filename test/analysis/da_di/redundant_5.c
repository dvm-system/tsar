int X;
int * foo() { return &X; }

int main() {
  int *P;
  int S = 0;
  // Note, that 'instcombine' after loop rotate moves call of foo() outside
  // the loop. Hence, usage of address of X inside foo() becomes redundant.
  // This allow to determine that X can be privitized in case of
  // corresponding transformation of a source code.
  for (int I = 0; I < 10; ++I) {    
    X = I;
    S += X;
    P = foo();
  }
  // P implicitly refencese X (via foo()) and X has been updated in a loop.
  return S + *P;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 redundant_5.c:11:3
//CHECK:    first private:
//CHECK:     <P:5:8, 8> | <X, 4>
//CHECK:    second to last private:
//CHECK:     <P:5:8, 8> | <X, 4>
//CHECK:    induction:
//CHECK:     <I:11:12, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:6:7, 4>:add
//CHECK:    redundant:
//CHECK:     <X, 4> foo():14:9
//CHECK:    lock:
//CHECK:     <I:11:12, 4>
//CHECK:    header access:
//CHECK:     <I:11:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:11:12, 4> | <P:5:8, 8> | <S:6:7, 4>
//CHECK:    address access:
//CHECK:     <X, 4> foo():14:9
//CHECK:    explicit access (separate):
//CHECK:     <I:11:12, 4> <P:5:8, 8> <S:6:7, 4> <X, 4>
//CHECK:    redundant (separate):
//CHECK:     foo():14:9
//CHECK:    lock (separate):
//CHECK:     <I:11:12, 4>
//CHECK:    address access (separate):
//CHECK:     foo():14:9
//CHECK:    direct access (separate):
//CHECK:     <I:11:12, 4> <P:5:8, 8> <S:6:7, 4> <X, 4> foo():14:9
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//REDUNDANT: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//REDUNDANT:  loop at depth 1 redundant_5.c:11:3
//REDUNDANT:    first private:
//REDUNDANT:     <P:5:8, 8> | <X, 4>
//REDUNDANT:    second to last private:
//REDUNDANT:     <P:5:8, 8> | <X, 4>
//REDUNDANT:    induction:
//REDUNDANT:     <I:11:12, 4>:[Int,0,10,1]
//REDUNDANT:    reduction:
//REDUNDANT:     <S:6:7, 4>:add
//REDUNDANT:    redundant:
//REDUNDANT:     <X, 4> foo():14:9
//REDUNDANT:    lock:
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    header access:
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    explicit access:
//REDUNDANT:     <I:11:12, 4> | <P:5:8, 8> | <S:6:7, 4> | <X, 4>
//REDUNDANT:    explicit access (separate):
//REDUNDANT:     <I:11:12, 4> <P:5:8, 8> <S:6:7, 4> <X, 4>
//REDUNDANT:    redundant (separate):
//REDUNDANT:     foo():14:9
//REDUNDANT:    lock (separate):
//REDUNDANT:     <I:11:12, 4>
//REDUNDANT:    direct access (separate):
//REDUNDANT:     <I:11:12, 4> <P:5:8, 8> <S:6:7, 4> <X, 4>
