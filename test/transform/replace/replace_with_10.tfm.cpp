typedef double T;

struct Data {
  T Value;
};

struct STy {
  T (*F)(T);
  int NX;
  Data *D;
};

struct UnusedTy {};

void foo(STy *S, UnusedTy *U, double Val);

/* Replacement for void foo(STy *S, UnusedTy *U, double Val) */
void foo_spf0(int S_NX0, struct Data *S_D0, T (*S_F0)(T), double Val) {
#pragma spf metadata replace(foo({.NX = S_NX0, .D = S_D0, .F = S_F0}, {}, Val))
  for (int I = 0; I < S_NX0; I += 2)
    S_D0[I].Value = S_D0[I + 1].Value = S_F0(Val);
}

void bar(STy *S) {
  UnusedTy *U;

  foo_spf0((S)->NX, (S)->D, (S)->F, 5.0);
}
