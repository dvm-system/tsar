#include <stdlib.h>

int* f3(int *a, int* b, int* c) {
	if (a[0] == b[0]) {
		return a + b[0];
	} else if (a[0] == c[0]) {
		return b + a[0];
	} else {
		return c + b[0];
	}
}

int f2(int *a, int* b) {
	return f3(a, b, a)[0] + 2;
}

int f1(int *a, int* b) {

	return f2(a, b);
}

int main(int argc, char const *argv[]) {
	int *a = malloc(sizeof(int) * 5);
	int b[10];
	a[0] = 5;
	a[1] = 6;
	a[2] = 7;
	b[0] = 10;
	b[1] = 20;
	b[2] = 30;

	return f1(a, b);
}