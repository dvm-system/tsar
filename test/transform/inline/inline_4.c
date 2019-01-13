extern int X;

void f() {
  X = 10;
}

int X;

void g() {
  X = 5;
  #pragma spf transform inline
  f();
}

//CHECK: 
