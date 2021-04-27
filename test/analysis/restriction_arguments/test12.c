#include <stdlib.h>

static int u[20];
static int v[20];

static int ir[10];

int foo(int *a, int* b) {
	return a[0] + b[0];
}

int main(int argc, char const *argv[]) {
	int k = 2;
	return foo(&u[ir[k]], &v[ir[k]]);
}