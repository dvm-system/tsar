#define MACRO(f_) f_() +

int f1() { return 0; }

int f2() { return MACRO(f1) 4; }
