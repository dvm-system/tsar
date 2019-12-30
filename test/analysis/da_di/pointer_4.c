typedef struct Factorization{
  long int *mlt;
  long int *exp;
  long int dim;
} Factorization;

int foo(Factorization **fctlist, int genexp){
  int lexp = 0;
  for(int k = 0; k < fctlist[genexp]->dim; k++){
    lexp -= fctlist[genexp]->exp[k];
  }
  return lexp;
}


//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 pointer_4.c:9:3
//CHECK:    induction:
//CHECK:     <k:9:11, 4>:[Int,0,,1]
//CHECK:    reduction:
//CHECK:     <lexp:8:7, 4>:add
//CHECK:    read only:
//CHECK:     <*fctlist:7:25, ?> <*fctlist[?]:{9:22|7:25}, 24> <*fctlist[?]:{9:22|9:22|7:25}, 24> <*fctlist[?][?].exp:{10:30|7:25}, ?> <?,?> <?:10:30,?> | <fctlist:7:25, 8> | <genexp:7:38, 4>
//CHECK:    lock:
//CHECK:     <*fctlist:7:25, ?> <*fctlist[?]:{9:22|7:25}, 24> <*fctlist[?]:{9:22|9:22|7:25}, 24> <*fctlist[?][?].exp:{10:30|7:25}, ?> <?,?> <?:10:30,?> | <fctlist:7:25, 8> | <genexp:7:38, 4> | <k:9:11, 4>
//CHECK:    header access:
//CHECK:     <*fctlist:7:25, ?> <*fctlist[?]:{9:22|7:25}, 24> <*fctlist[?]:{9:22|9:22|7:25}, 24> <*fctlist[?][?].exp:{10:30|7:25}, ?> <?,?> <?:10:30,?> | <fctlist:7:25, 8> | <genexp:7:38, 4> | <k:9:11, 4>
//CHECK:    explicit access:
//CHECK:     <fctlist:7:25, 8> | <genexp:7:38, 4> | <k:9:11, 4> | <lexp:8:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <fctlist:7:25, 8> <genexp:7:38, 4> <k:9:11, 4> <lexp:8:7, 4>
//CHECK:    lock (separate):
//CHECK:     <*fctlist:7:25, ?> <*fctlist[?]:{9:22|7:25}, 24> <fctlist:7:25, 8> <genexp:7:38, 4> <k:9:11, 4>
//CHECK:    direct access (separate):
//CHECK:     <*fctlist:7:25, ?> <*fctlist[?]:{9:22|7:25}, 24> <*fctlist[?][?].exp:{10:30|7:25}, ?> <fctlist:7:25, 8> <genexp:7:38, 4> <k:9:11, 4> <lexp:8:7, 4>
//CHECK:    indirect access (separate):
//CHECK:     <?,?> <?:10:30,?>
