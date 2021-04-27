#include <stdlib.h>

int f2(int *a, int* b) {
	return a[0] + b[0] + 2;
}

int foo(int *a, int* b, int* c) {
	return f2(a, c) + f2(b, c);
}


static int u[20];
static int v[20];

int *bar() {
	return u;
}

int *yaf() {
	return v;
}

int main(int argc, char const *argv[]) {
	int *a = bar();

	int *b = bar();

	int *c = yaf();
	//Not restrict calls:
	int r1 = foo(a, b, c);

	return r1;
}