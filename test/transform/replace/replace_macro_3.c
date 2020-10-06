struct STy { int X; };

#define STyS (struct STy *S

int foo STyS, int X) {
 #pragma spf transform replace(S) nostrict
 return S->X = X;
}

//CHECK: replace_macro_3.c:5:5: warning: disable structure replacement
//CHECK: int foo STyS, int X) {
//CHECK:     ^
//CHECK: replace_macro_3.c:5:9: note: macro prevent replacement
//CHECK: int foo STyS, int X) {
//CHECK:         ^
//CHECK: replace_macro_3.c:3:15: note: expanded from macro 'STyS'
//CHECK: #define STyS (struct STy *S
//CHECK:               ^
//CHECK: 1 warning generated.
