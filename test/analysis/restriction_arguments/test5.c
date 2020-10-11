#include <stdlib.h>

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int *a = malloc(sizeof(int) * 5);
	a[0] = 5;
	a[1] = 6;
	a[2] = 7;
	int b[10];
	b[0] = 10;
	b[1] = 20;
	b[2] = 30;

	//Restrict calls:
	int r1 = foo(a, b);
	int r2 = foo(b, a);

	int *aOffset = a + 2;
	int *bOffset = &(b[1]);

	//Unresctrict calls:
	int r3 = foo(a, a);
	int r4 = foo(b, b);
	int r5 = foo(a, aOffset);
	int r6 = foo(bOffset, b);
	
	return r1 + r2 + r3 + r4 + r5 + r6;
}