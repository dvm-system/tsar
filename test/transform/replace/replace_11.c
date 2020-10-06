struct STy { int X; };
struct DTy { struct STy S; };

#define STy struct STy
int foo(struct DTy *D, int X) {
 _Pragma("spf transform replace(D) nostrict")
 return D->S.X = X;
}

//CHECK: replace_11.c:5:21: warning: disable structure replacement
//CHECK: int foo(struct DTy *D, int X) {
//CHECK:                     ^
//CHECK: replace_11.c:2:14: note: unable to build declaration of a record member
//CHECK: struct DTy { struct STy S; };
//CHECK:              ^
//CHECK: 1 warning generated.
