#include <stdlib.h>

int f2(char *a, char* b) {
	return a[0] + b[0] + 2;
}

int foo(char *a, char* b, char* c) {
	return f2(a, c) + f2(b, c);
}

char *bar() {
	return "BARRRR";
}

char *yaf() {
	return "YAFFFF";
}

int main(int argc, char const *argv[]) {
	char *a = bar();

	char *b = bar();

	char *c = yaf();
	//Not restrict calls:
	int r1 = foo(a, b, c);

	return r1;
}