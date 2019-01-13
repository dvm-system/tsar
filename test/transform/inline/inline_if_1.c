int f() { return 1; }

int main() {
  #pragma spf transform inline
  {
    if (f())
      return 0;
    return 1;
  }
}
//CHECK: 
