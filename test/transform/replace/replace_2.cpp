typedef double T;

struct Data {
  T Value;
};

struct STy {
  T (*F)(T);
  int NX;
  Data *D;
};

struct UnusedTy { };

void foo(STy *S, UnusedTy *U, double Val) {
  #pragma spf transform replace(S, U) nostrict
  for (int I = 0; I < S->NX; I += 2)
    S->D[I].Value = S->D[I + 1].Value = S->F(Val);
}
//CHECK: replace_2.cpp:15:28: remark: structure replacement
//CHECK: void foo(STy *S, UnusedTy *U, double Val) {
//CHECK:                            ^
//CHECK: replace_2.cpp:15:18: remark: remove unused declaration
//CHECK: void foo(STy *S, UnusedTy *U, double Val) {
//CHECK:                  ^
