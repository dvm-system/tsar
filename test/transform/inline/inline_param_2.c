void f(void (*h)(int), int I) {
#pragma spf transform inline
  h(I);
}

void g(int I) {
#pragma spf transform inline
  f(g, I);
}
//CHECK: inline_param_2.c:3:3: warning: disable inline expansion for function without definition
//CHECK:   h(I);
//CHECK:   ^
//CHECK: 1 warning generated.
