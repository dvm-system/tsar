#define T int
struct STy { T X; };

#define STy struct STy
int foo(STy *S, T X) {
 _Pragma("spf transform replace(S) nostrict")
 return S->X + X;
}

//CHECK: 
