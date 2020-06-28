struct STy { int X; };

#define STy struct STy
int foo(STy *S) {
 _Pragma("spf transform replace(S) nostrict")
 return S->X;
}

//CHECK: 
