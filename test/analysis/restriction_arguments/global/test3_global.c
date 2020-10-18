#include <stdlib.h>

int b[10][20];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int *a1 = malloc(sizeof(int) * 5);
	int (*a2)[10] = malloc(sizeof(int) * 10 * 20);
	return foo(a1, b) + foo(a2[2], b);
}