int f1() { return 0; }

#define MACRO +x

void f2() {
  int x;

  f1() MACRO;
}
