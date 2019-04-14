int f() { return 0; }

#define MACRO x + x;

void f2() {
  int x;

  MACRO f();
}
