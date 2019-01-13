int f() { return 5; }

int main() {
  #pragma spf transform inline
  {
    int X;
    if (1)
      X = f();
    return X;
  }
}
//CHECK: 
