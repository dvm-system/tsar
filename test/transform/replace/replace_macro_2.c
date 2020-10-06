struct STy { int X; };

#define STyS struct STy *S

int foo(STyS, int X) {
 #pragma spf transform replace(S) nostrict
 return S->X = X;
}

//CHECK: 
