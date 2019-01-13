void f(int *X) { *X = 10; }

void g() {
  int Y;
  _Pragma("spf transform inline propagate")
  f(&Y);
}
//CHECK: 
