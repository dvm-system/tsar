#include <stdlib.h>

int foo(char *a, char* b) {
	return a[0] + b[0];
}

char *bar(int a) {
	return "BARRRR";
}

int main(int argc, char const *argv[]) {
	char *a = bar(0);

	char *b = bar(1);
	//Not restrict calls:
	int r1 = foo(a, b);
	int r2 = foo(b, a);

	return r1 + r2;
}