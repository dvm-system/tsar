#include <stdlib.h>

const int c = 5;
static int n;
int b[10];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	n = 20;
	int (*a)[10] = malloc(sizeof(int) * 10 * n * c);
	return foo(a[2], b);
}