void test(int *a) {
    for (int j = 0; j < 10; ++j) {
        int b[2];
        *a = b[j];
    }
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'test':
//CHECK:  loop at depth 1 private_array_10.c:2:5
//CHECK:    shared:
//CHECK:     <b:3:13, 8>
//CHECK:    first private:
//CHECK:     <a[0]:1:16, 4>
//CHECK:    second to last private:
//CHECK:     <a[0]:1:16, 4>
//CHECK:    private:
//CHECK:     <b:3:13, 8>
//CHECK:    induction:
//CHECK:     <j:2:14, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <a:1:16, 8>
//CHECK:    lock:
//CHECK:     <j:2:14, 4>
//CHECK:    header access:
//CHECK:     <j:2:14, 4>
//CHECK:    explicit access:
//CHECK:     <a:1:16, 8> | <j:2:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <a:1:16, 8> <a[0]:1:16, 4> <j:2:14, 4>
//CHECK:    lock (separate):
//CHECK:     <j:2:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <a:1:16, 8> <a[0]:1:16, 4> <b:3:13, 8> <j:2:14, 4>
