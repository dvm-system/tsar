int main() {
  int I;
  for (I = 0; I < 10; ++I) {
    int X = I;   
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'main':
//CHECK:  loop at depth 1 private_9.c:3:3
//CHECK:    private:
//CHECK:     <X:4:9, 4>
//CHECK:    induction:
//CHECK:     <I:2:7, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:2:7, 4>
//CHECK:    header access:
//CHECK:     <I:2:7, 4>
//CHECK:    explicit access:
//CHECK:     <I:2:7, 4> | <X:4:9, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:2:7, 4> <X:4:9, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:7, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:2:7, 4> <X:4:9, 4>
