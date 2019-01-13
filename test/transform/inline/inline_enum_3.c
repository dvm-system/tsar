typedef enum { false, true } logical;

void f(logical *L);

void f1(logical *L) {}

int main() {
  logical L;
#pragma spf transform inline
  f(&L);
  return 0;
}
//CHECK: 
//CHECK-1: 
