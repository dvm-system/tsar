int Start, End;

void foo(int * restrict A) {
  for (int I = End - 1; I >= Start; --I)
    A[I] = I;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 induction_1.c:4:3
//CHECK:    shared:
//CHECK:     <*A:3:25, ?>
//CHECK:    first private:
//CHECK:     <*A:3:25, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:3:25, ?>
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,,,-1]
//CHECK:    read only:
//CHECK:     <A:3:25, 8> | <Start, 4>
//CHECK:    lock:
//CHECK:     <I:4:12, 4> | <Start, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4> | <Start, 4>
//CHECK:    explicit access:
//CHECK:     <A:3:25, 8> | <I:4:12, 4> | <Start, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:3:25, 8> <I:4:12, 4> <Start, 4>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4> <Start, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:3:25, ?> <A:3:25, 8> <I:4:12, 4> <Start, 4>
