void test(int *a) {
    for (int j = 0; j < 10; ++j) {
        int b[2];
        for (int i = 0; i < 2; ++i) {
            b[i] = i;
        }
        a[0] = b[0];
    }
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'test':
//CHECK:  loop at depth 1 private_array_8.c:2:5
//CHECK:    private:
//CHECK:     <b:3:13, 8> | <i:4:18, 4>
//CHECK:    output:
//CHECK:     <*a:{7:9|1:16}, ?> <a[0]:1:16, 4>
//CHECK:    anti:
//CHECK:     <*a:{7:9|1:16}, ?> <a[0]:1:16, 4>
//CHECK:    flow:
//CHECK:     <*a:{7:9|1:16}, ?> <a[0]:1:16, 4>
//CHECK:    induction:
//CHECK:     <j:2:14, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <a:1:16, 8>
//CHECK:    redundant:
//CHECK:     <*a:{7:9|1:16}, ?> <a[0]:1:16, 4>
//CHECK:    lock:
//CHECK:     <j:2:14, 4>
//CHECK:    header access:
//CHECK:     <j:2:14, 4>
//CHECK:    explicit access:
//CHECK:     <a:1:16, 8> | <i:4:18, 4> | <j:2:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <a:1:16, 8> <a[0]:1:16, 4> <i:4:18, 4> <j:2:14, 4>
//CHECK:    redundant (separate):
//CHECK:     <*a:{7:9|1:16}, ?>
//CHECK:    lock (separate):
//CHECK:     <j:2:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <*a:{7:9|1:16}, ?> <a:1:16, 8> <a[0]:1:16, 4> <b:3:13, 8> <i:4:18, 4> <j:2:14, 4>
//CHECK:   loop at depth 2 private_array_8.c:4:9
//CHECK:     shared:
//CHECK:      <b:3:13, 8>
//CHECK:     first private:
//CHECK:      <b:3:13, 8>
//CHECK:     dynamic private:
//CHECK:      <b:3:13, 8>
//CHECK:     induction:
//CHECK:      <i:4:18, 4>:[Int,0,2,1]
//CHECK:     lock:
//CHECK:      <i:4:18, 4>
//CHECK:     header access:
//CHECK:      <i:4:18, 4>
//CHECK:     explicit access:
//CHECK:      <i:4:18, 4>
//CHECK:     explicit access (separate):
//CHECK:      <i:4:18, 4>
//CHECK:     lock (separate):
//CHECK:      <i:4:18, 4>
//CHECK:     direct access (separate):
//CHECK:      <b:3:13, 8> <i:4:18, 4>
