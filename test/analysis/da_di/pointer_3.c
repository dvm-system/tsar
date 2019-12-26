void bar(int *);

int foo(int **V, int N) {
  int *Buffer = *V;
  for(int I = 0; I < N; I++ )
    bar(Buffer);	
  return 0;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_3.c:5:3
//CHECK:    output:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5
//CHECK:    anti:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5
//CHECK:    flow:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5
//CHECK:    induction:
//CHECK:     <I:5:11, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <Buffer:4:8, 8> | <N:3:22, 4>
//CHECK:    redundant:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5
//CHECK:    lock:
//CHECK:     <I:5:11, 4> | <N:3:22, 4>
//CHECK:    header access:
//CHECK:     <I:5:11, 4> | <N:3:22, 4>
//CHECK:    explicit access:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5 | <Buffer:4:8, 8> | <I:5:11, 4> | <N:3:22, 4>
//CHECK:    address access:
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> bar():6:5
//CHECK:    explicit access (separate):
//CHECK:     <*Buffer:{6:9|4:8}, ?> <*V[0]:{4:17|3:15}, ?> <Buffer:4:8, 8> <I:5:11, 4> <N:3:22, 4> bar():6:5
//CHECK:    redundant (separate):
//CHECK:     <*Buffer:{6:9|4:8}, ?>
//CHECK:    lock (separate):
//CHECK:     <I:5:11, 4> <N:3:22, 4>
//CHECK:    address access (separate):
//CHECK:     bar():6:5
//CHECK:    direct access (separate):
//CHECK:     <*Buffer:{6:9|4:8}, ?> <Buffer:4:8, 8> <I:5:11, 4> <N:3:22, 4> bar():6:5
//CHECK:    indirect access (separate):
//CHECK:     <*V[0]:{4:17|3:15}, ?>
