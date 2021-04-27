#include <stdlib.h>

static int u[20];
static int v[20];

static int ir[3][10];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int l = 1;
	int k = 2;
	return foo(&u[ir[l][k]], &v[ir[l][k]]);
}