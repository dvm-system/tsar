int main() {
  int I, X;
  I = 0;
  do {
    X = I;
    ++I;
  } while (I < 10);
  X = X + 1;
  return X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_10.c:4:3
//CHECK:    last private:
//CHECK:     <X:2:10, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,9,1]
//CHECK:    lock:
//CHECK:     <I:2:7, 4> | <X:2:10, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:2:10, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:2:10, 4>
