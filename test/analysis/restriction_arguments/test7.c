#include <stdlib.h>

int foo(char *a, char* b) {
	return a[0] + b[0];
}

char* bar(char *a, char* b) {
	return a + 2;
}

int main(int argc, char const *argv[]) {
	char *a = "HELLO";

	char *b = "HI";
	//Restrict calls:
	int r1 = foo(a, b);
	int r2 = foo(bar(a, b), b);
	//Non-restrict
	int r3 = foo(bar(a, b), a);

	return r1 + r2 + r3;
	// return r1 + bar(a, b);
}