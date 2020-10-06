int main() {
  int I, X;
  I = 0;
  while (I < 10) {
    X = I;
    ++I;
  };
  X = X + 1;
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_11.c:4:3
//CHECK:    second to last private:
//CHECK:     <X:2, 4>
//CHECK:    induction:
//CHECK:     <I:2, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:2, 4>
//CHECK:    header access:
//CHECK:     <I:2, 4>
//CHECK:    explicit access:
//CHECK:     <I:2, 4> | <X:2, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2, 4> <X:2, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2, 4> <X:2, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//SAFE:  loop at depth 1 private_11.c:4:3
//SAFE:    first private:
//SAFE:     <X:2, 4>
//SAFE:    second to last private:
//SAFE:     <X:2, 4>
//SAFE:    induction:
//SAFE:     <I:2, 4>:[Int,0,10,1]
//SAFE:    lock:
//SAFE:     <I:2, 4>
//SAFE:    header access:
//SAFE:     <I:2, 4>
//SAFE:    explicit access:
//SAFE:     <I:2, 4> | <X:2, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:2, 4> <X:2, 4>
//SAFE:    lock (separate):
//SAFE:     <I:2, 4>
//SAFE:    direct access (separate):
//SAFE:     <I:2, 4> <X:2, 4>
