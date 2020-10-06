struct STy { int X; };

#define CALL bar(S);

void bar(struct STy *S) {
  S->X = S->X + 1;
}

int foo (struct STy *S) {
 #pragma spf transform replace(S) nostrict
 CALL
 return S->X;
}

//CHECK: replace_macro_7.c:9:5: warning: disable structure replacement
//CHECK: int foo (struct STy *S) {
//CHECK:     ^
//CHECK: replace_macro_7.c:11:2: note: macro prevent replacement
//CHECK:  CALL
//CHECK:  ^
//CHECK: replace_macro_7.c:3:19: note: expanded from macro 'CALL'
//CHECK: #define CALL bar(S);
//CHECK:                   ^
//CHECK: replace_macro_7.c:9:10: warning: disable structure replacement
//CHECK: int foo (struct STy *S) {
//CHECK:          ^
//CHECK: replace_macro_7.c:11:2: note: not-arrow access prevent replacement
//CHECK:  CALL
//CHECK:  ^
//CHECK: replace_macro_7.c:3:18: note: expanded from macro 'CALL'
//CHECK: #define CALL bar(S);
//CHECK:                  ^
//CHECK: 2 warnings generated.
