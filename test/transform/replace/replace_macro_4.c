struct STy { int X; };

#define _S_ S->X = X

int foo (struct STy *S, int X) {
 #pragma spf transform replace(S) nostrict
 return _S_;
}

//CHECK: replace_macro_4.c:5:5: warning: disable structure replacement
//CHECK: int foo (struct STy *S, int X) {
//CHECK:     ^
//CHECK: replace_macro_4.c:7:9: note: macro prevent replacement
//CHECK:  return _S_;
//CHECK:         ^
//CHECK: replace_macro_4.c:3:16: note: expanded from macro '_S_'
//CHECK: #define _S_ S->X = X
//CHECK:                ^
//CHECK: 1 warning generated.
