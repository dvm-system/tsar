#include <stdlib.h>

int foo(char *a, char* b) {
	return a[0] + b[0];
}

char *bar() {
	return "BARRRR";
}

int main(int argc, char const *argv[]) {
	char *a = bar();

	char *b = bar();
	//Not restrict calls:
	int r1 = foo(a, b);
	int r2 = foo(b, a);

	return r1 + r2;
}