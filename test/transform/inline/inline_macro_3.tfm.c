void f() { int X; }

#define MACRO(f_) f_();

void f1() { MACRO(f) }
