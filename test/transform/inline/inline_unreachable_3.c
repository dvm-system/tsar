int f() {return 3;}

void g() {
  #pragma spf transform inline
  0 ? f() : 4;
}

void h() {
  #pragma spf transform inline
  g();
}
//CHECK: inline_unreachable_3.c:5:7: warning: disable inline expansion of unreachable call
//CHECK:   0 ? f() : 4;
//CHECK:       ^
//CHECK: 1 warning generated.
