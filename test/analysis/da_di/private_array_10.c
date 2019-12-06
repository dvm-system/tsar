int test(int *a) {
    for (int j = 0; j < 2; ++j) {
        int b[3] = {1, 2, 3};
        b[j] *= 2;
        *a = b[j];
    }
    return *a;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'test':
//CHECK:  loop at depth 1 private_array_10.c:2:5
//CHECK:    private:
//CHECK:     <b:3:13, 12>
//CHECK:    output:
//CHECK:     <a[0]:1:15, 4> <sapfor.var, 12>
//CHECK:    anti:
//CHECK:     <a[0]:1:15, 4> <sapfor.var, 12>
//CHECK:    flow:
//CHECK:     <a[0]:1:15, 4> <sapfor.var, 12>
//CHECK:    induction:
//CHECK:     <j:2:14, 4>:[Int,0,2,1]
//CHECK:    read only:
//CHECK:     <a:1:15, 8>
//CHECK:    lock:
//CHECK:     <j:2:14, 4>
//CHECK:    header access:
//CHECK:     <j:2:14, 4>
//CHECK:    explicit access:
//CHECK:     <a:1:15, 8> | <a[0]:1:15, 4> <sapfor.var, 12> | <b:3:13, 12> | <j:2:14, 4>
//CHECK:    address access:
//CHECK:     <a[0]:1:15, 4> <sapfor.var, 12> | <b:3:13, 12>
//CHECK:    explicit access (separate):
//CHECK:     <a:1:15, 8> <a[0]:1:15, 4> <b:3:13, 12> <j:2:14, 4> <sapfor.var, 12>
//CHECK:    lock (separate):
//CHECK:     <j:2:14, 4>
//CHECK:    address access (separate):
//CHECK:     <b:3:13, 12> <sapfor.var, 12>
//CHECK:    direct access (separate):
//CHECK:     <a:1:15, 8> <a[0]:1:15, 4> <b:3:13, 12> <j:2:14, 4> <sapfor.var, 12>
