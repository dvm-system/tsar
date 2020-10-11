#include <stdlib.h>

int b[10][20];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int *a = malloc(sizeof(int) * 5);
	return foo(a, b[0]);
}