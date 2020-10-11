#include <stdlib.h>

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int *a = malloc(sizeof(int) * 5);
	int b[10][20];
	int *bPtr;
	bPtr = (int*) b[0];
	return foo(a, bPtr);
}