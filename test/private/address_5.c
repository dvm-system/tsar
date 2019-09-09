void init(int **P);
void exec(int *P);

void bar() {
  int *P;
  int S;
  for (int I = 0; I < 10; ++I) {    
    init(&P);
    exec(P);
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 address_5.c:7:3
//CHECK:    output:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    anti:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    flow:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5 | <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5 | <I:7:12, 4>
//CHECK:    address access:
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    explicit access (separate):
//CHECK:     <*P:{9:10|5:8}, ?> <I:7:12, 4> <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    lock (separate):
//CHECK:     <*P:{9:10|5:8}, ?> <I:7:12, 4> <P:5:8, ?>
//CHECK:    address access (separate):
//CHECK:     <P:5:8, ?> exec():9:5 init():8:5
//CHECK:    no promoted scalar (separate):
//CHECK:     <*P:{9:10|5:8}, ?> <P:5:8, ?>
//CHECK:    direct access (separate):
//CHECK:     <*P:{9:10|5:8}, ?> <I:7:12, 4> <P:5:8, ?> exec():9:5 init():8:5
