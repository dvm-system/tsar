#include <stdlib.h>

int (*b)[10];
int *a1;
int (*a2)[10];

int foo(int *a, int* b) {
	return a[0] + bar(a, b);
}

int bar(int *a, int* b) {
	return a[5] + 5 + b[7];
}

int main(int argc, char const *argv[]) {
	a1 = malloc(sizeof(int) * 5);
	a2 = malloc(sizeof(int) * 10 * 20);
	b = malloc(sizeof(int) * 10 * 20);
	return foo(a1, b[3]) + foo(a2[2], b[3]);
}