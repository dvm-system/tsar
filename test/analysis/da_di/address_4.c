int X;

void foo(long long * restrict A) {
 for (int I = 0; I < 10; ++I)
    A[I] = ((long long) &X) + 1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 address_4.c:4:2
//CHECK:    shared:
//CHECK:     <*A:3, ?>
//CHECK:    first private:
//CHECK:     <*A:3, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:3, ?>
//CHECK:    induction:
//CHECK:     <I:4[4:2], 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <A:3, 8>
//CHECK:    lock:
//CHECK:     <I:4[4:2], 4>
//CHECK:    header access:
//CHECK:     <I:4[4:2], 4>
//CHECK:    explicit access:
//CHECK:     <A:3, 8> | <I:4[4:2], 4>
//CHECK:    address access:
//CHECK:     <X, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:3, 8> <I:4[4:2], 4>
//CHECK:    lock (separate):
//CHECK:     <I:4[4:2], 4>
//CHECK:    address access (separate):
//CHECK:     <X, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:3, ?> <A:3, 8> <I:4[4:2], 4> <X, 4>
