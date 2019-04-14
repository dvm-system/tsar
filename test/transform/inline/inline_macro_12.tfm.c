#define M +
int f() { return 5 M 5; }

void f1() {

  { f(); }
}
