int f() { return 5; }
int main() {
  #pragma spf transform inline
  struct A { int X; } A1 = { .X = f() };
  struct A A2;
}
//CHECK: 
