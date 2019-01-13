int X;

void f() {
  X = 4;
}

void g() {
  #pragma spf transform inline
  f();
}

int h() {
  int X = 5;
  #pragma spf transform inline
  g();
  return X;
}
//CHECK: inline_3.c:9:3: warning: disable inline expansion
//CHECK:   f();
//CHECK:   ^
//CHECK: inline_3.c:1:5: note: hidden external dependence prevents inlining
//CHECK: int X;
//CHECK:     ^
//CHECK: inline_3.c:13:7: note: declaration hides other declaration
//CHECK:   int X = 5;
//CHECK:       ^
//CHECK: 1 warning generated.
