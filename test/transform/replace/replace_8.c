struct STy { int X; };

#define FOO \
int foo(struct STy *S) { \
 _Pragma("spf transform replace(S) nostrict") \
 return S->X; \
}

FOO
//CHECK: replace_8.c:9:1: warning: unable to remove directive in macro
//CHECK: FOO
//CHECK: ^
//CHECK: replace_8.c:5:2: note: expanded from macro 'FOO'
//CHECK:  _Pragma("spf transform replace(S) nostrict") \
//CHECK:  ^
//CHECK: <scratch space>:2:16: note: expanded from here
//CHECK:  spf transform replace(S) nostrict
//CHECK:                ^
//CHECK: replace_8.c:9:1: warning: disable structure replacement
//CHECK: replace_8.c:4:5: note: expanded from macro 'FOO'
//CHECK: int foo(struct STy *S) { \
//CHECK:     ^
//CHECK: replace_8.c:9:1: note: macro prevent replacement
//CHECK: replace_8.c:4:5: note: expanded from macro 'FOO'
//CHECK: int foo(struct STy *S) { \
//CHECK:     ^
//CHECK: 2 warnings generated.
