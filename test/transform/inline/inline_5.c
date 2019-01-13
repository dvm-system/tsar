int f() { return 0; }

void f1() {
  for (int I = 0; I < 10; ++I) {
    #pragma spf transform inline
    f();
 }
}

void f2() {
  #pragma spf transform inline
  for (int I = 0; I < 10; I = I + f());
}

void f3() {
  #pragma spf transform inline
  for (int I = 0; I < f(); ++I);
}

void f4() {
  #pragma spf transform inline
  for (int I = f(); I < 10; ++I);
}
//CHECK: inline_5.c:12:35: warning: disable inline expansion in the third section of for-loop
//CHECK:   for (int I = 0; I < 10; I = I + f());
//CHECK:                                   ^
//CHECK: inline_5.c:17:23: warning: disable inline expansion in conditional expression of loop
//CHECK:   for (int I = 0; I < f(); ++I);
//CHECK:                       ^
//CHECK: 2 warnings generated.
