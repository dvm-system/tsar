int B[10];

void foo(int X, char Y) {
  double A[2];
  A[0] = X;
  A[1] = X;
  *((char *)A + 1) = Y;
  for (int I = 0; I < 10; ++I)
    B[I] = A[0] + A[1];  
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 address_2.c:8:3
//CHECK:    shared:
//CHECK:     <B, 40>
//CHECK:    first private:
//CHECK:     <B, 40>
//CHECK:    dynamic private:
//CHECK:     <B, 40>
//CHECK:    induction:
//CHECK:     <I:8:12, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <A:4:10, 16>
//CHECK:    lock:
//CHECK:     <I:8:12, 4>
//CHECK:    header access:
//CHECK:     <I:8:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:8:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:8:12, 4>
//CHECK:    lock (separate):
//CHECK:     <I:8:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <A:4:10, 16> <B, 40> <I:8:12, 4>
