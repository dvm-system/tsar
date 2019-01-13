void f(int X) {
}

void g(int X) {
  _Pragma("spf transform inline")
  f(X)

  ;
}

void h() {
  int X0, X1;
  #pragma spf transform inline
  g(X1);
}
//CHECK: 
