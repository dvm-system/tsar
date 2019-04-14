int f() { return 4; }

#define MACRO(x) x +

void f1() {
  int x;

  MACRO(x) f();
}
