int f(int);

void f1() {
#pragma spf transform inline
  f(5);
}


int A;

int f(int A) {
  return A;
}
//CHECK: 
