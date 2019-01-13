struct A { int X; };

int f(int (*g)(struct A)) {
  struct A A1;
  A1.X = 5;
  return g(A1);
}

int g(struct A);

void f1() {
  #pragma spf transform inline
  f(g);
}
//CHECK: 
