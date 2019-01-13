int f() { return 1; }

int main() {
  #pragma spf transform inline
  {
      if (1)
        f();
      return 1;
  }
}
//CHECK: 
