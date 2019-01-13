void f(int *x) { *x = *x + 1; }

int f1(int *x) { return *x = 1, f(x), *x; }
