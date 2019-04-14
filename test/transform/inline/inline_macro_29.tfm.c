#define DOT .
#define L [
#define R ]
#define EQ =
#define RANGE ...
#define _4 4
#define _6 6
#define _5 5

struct A {
  int X[10];
};

void f_DOT() { struct A A1 = {DOT X[4 ... 6] = 5}; }

void f_L() {
  struct A A1 = { . X L 4 ... 6 ] = 5 };
}

void f_R() {
  struct A A1 = { . X [ 4 ... 6 R = 5 };
}

void f_EQ() {
  struct A A1 EQ {
    .X[4 ... 6] = 5
  };
}

void f_RANGE() { struct A A1 = {.X[4 RANGE 6] = 5}; }

void f_4() { struct A A1 = {.X[_4... 6] = 5}; }

void f_6() { struct A A1 = {.X[4 ... _6] = 5}; }

void f_5() { struct A A1 = {.X[4 ... 6] = _5}; }

void all() {

  {
    f_DOT();
    f_L();
    f_R();
    f_EQ();
    f_RANGE();
    f_4();
    f_6();
    f_5();
  }
}
