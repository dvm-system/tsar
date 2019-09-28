int main() {
  int I, X;
  for (I = 0; I < 10; ++I)
    X = I;
  X = 2 * X;
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_2.c:3:3
//CHECK:    second to last private:
//CHECK:     <X:2:10, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//SAFE:  loop at depth 1 private_2.c:3:3
//SAFE:    first private:
//SAFE:     <X:2:10, 4>
//SAFE:    second to last private:
//SAFE:     <X:2:10, 4>
//SAFE:    induction:
//SAFE:     <I:2:7, 4>:[Int,0,10,1]
//SAFE:    lock:
//SAFE:     <I:2:7, 4>
//SAFE:    header access:
//SAFE:     <I:2:7, 4>
//SAFE:    explicit access:
//SAFE:     <I:2:7, 4> | <X:2:10, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:2:7, 4> <X:2:10, 4>
//SAFE:    lock (separate):
//SAFE:     <I:2:7, 4>
//SAFE:    direct access (separate):
//SAFE:     <I:2:7, 4> <X:2:10, 4>
