typedef enum { false, true } logical;

logical f(logical L) {
  return L;
}

int main() {
#pragma spf transform inline
  return f(false);
}
//CHECK: 
