#include <stdlib.h>

int b[10];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int (*a)[10] = malloc(sizeof(int) * 10 * 20);
	return foo(a[2], b);
}