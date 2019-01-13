int f1();

int f2() { return f1(); }

int f1() { return f2() + 1; }
