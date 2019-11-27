#include <inttypes.h>

void test(int *a) {
    for (int j = 0; j < 10; ++j) {
        uint8_t b[2][2][2];
        for (int i = 0; i < 2; ++i) {
            b[1][1][i] = i;
        }
        a[0] = b[0][0][j];
    }
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'test':
//CHECK:  loop at depth 1 private_array_9.c:4:5
//CHECK:    private:
//CHECK:     <b:{5:9|5:17}, 8> | <i:6:18, 4>
//CHECK:    output:
//CHECK:     <*a:{9:9|3:16}, ?> <a[0]:3:16, 4>
//CHECK:    anti:
//CHECK:     <*a:{9:9|3:16}, ?> <a[0]:3:16, 4>
//CHECK:    flow:
//CHECK:     <*a:{9:9|3:16}, ?> <a[0]:3:16, 4>
//CHECK:    induction:
//CHECK:     <j:4:14, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <a:3:16, 8>
//CHECK:    redundant:
//CHECK:     <*a:{9:9|3:16}, ?> <a[0]:3:16, 4>
//CHECK:    lock:
//CHECK:     <j:4:14, 4>
//CHECK:    header access:
//CHECK:     <j:4:14, 4>
//CHECK:    explicit access:
//CHECK:     <a:3:16, 8> | <i:6:18, 4> | <j:4:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <a:3:16, 8> <a[0]:3:16, 4> <i:6:18, 4> <j:4:14, 4>
//CHECK:    redundant (separate):
//CHECK:     <*a:{9:9|3:16}, ?>
//CHECK:    lock (separate):
//CHECK:     <j:4:14, 4>
//CHECK:    direct access (separate):
//CHECK:     <*a:{9:9|3:16}, ?> <a:3:16, 8> <a[0]:3:16, 4> <b:{5:9|5:17}, 8> <i:6:18, 4> <j:4:14, 4>
//CHECK:   loop at depth 2 private_array_9.c:6:9
//CHECK:     shared:
//CHECK:      <b:{5:9|5:17}, 8>
//CHECK:     first private:
//CHECK:      <b:{5:9|5:17}, 8>
//CHECK:     dynamic private:
//CHECK:      <b:{5:9|5:17}, 8>
//CHECK:     induction:
//CHECK:      <i:6:18, 4>:[Int,0,2,1]
//CHECK:     lock:
//CHECK:      <i:6:18, 4>
//CHECK:     header access:
//CHECK:      <i:6:18, 4>
//CHECK:     explicit access:
//CHECK:      <i:6:18, 4>
//CHECK:     explicit access (separate):
//CHECK:      <i:6:18, 4>
//CHECK:     lock (separate):
//CHECK:      <i:6:18, 4>
//CHECK:     direct access (separate):
//CHECK:      <b:{5:9|5:17}, 8> <i:6:18, 4>
