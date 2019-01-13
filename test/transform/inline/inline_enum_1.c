enum logical { false, true };

enum logical f(enum logical L) {
  return L;
}

int main() {
#pragma spf transform inline
  return f(false);
}
//CHECK: 
