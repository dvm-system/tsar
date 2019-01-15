int X;
void f() { X = 5; }

void f1() {
  int X;
  #pragma spf transform inline
  f();
}
//CHECK: inline_36.c:7:3: warning: disable inline expansion
//CHECK:   f();
//CHECK:   ^
//CHECK: inline_36.c:1:5: note: hidden external dependence prevents inlining
//CHECK: int X;
//CHECK:     ^
//CHECK: inline_36.c:5:7: note: declaration hides other declaration
//CHECK:   int X;
//CHECK:       ^
//CHECK: 1 warning generated.
