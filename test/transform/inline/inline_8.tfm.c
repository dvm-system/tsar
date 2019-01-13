int f2() { return 2; }
int f1() { return 1 + (0 || f2()); }
