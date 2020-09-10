/*
Printing analysis 'address-access':
Function [fun7]: 	a,
Function [fun4]: 	a,
Function [fun6]: 	b,
Function [fun2]: 	b,
Function [fun1]: 	a,
Function [fun8]: 	a,
Function [fun5]: 	a,
Function [fun3]: 	b,
Function [main]:
Function [getAddr]:
 */

int *glob;

void fun1(int *a, int *b) {  // Function [fun1]: 	a,
    b[11] = b[12];
    glob = a;
}

int* fun2(int *a, int *b) {  // Function [fun2]: 	b,
    return b;
}

int* fun3(int *a, int *b) {  // Function [fun3]: 	b,
    int *c = b;
    return c;
}

void fun4(int *a, int *b) {  // Function [fun4]: 	a,
    int *d = &(b[10]);
    int *c = &(a[10]);
    glob = c;
}

void fun5(int *a, int **b) {  // Function [fun5]: 	a,
    int *c = &(a[10]);
    *b = c;
}

int **addr = (int **) 11231231;
int **getAddr() {
    return addr;
}

void fun6(int *a, int *b) {  // Function [fun6]: 	b,
    int **addr = getAddr();
    *addr = b;
}

void fun7(int *a, int *b) {  // Function [fun7]: 	a,
    fun6(b, a);
}

void fun8(int *a, int *b) {  // Function [fun8]: 	a,
    b[1] = b[2];
    int **c = &a;
    glob = *c;
}


int main() {
    int x[20];
    int **y;

    fun1(x, x);
    fun2(x, x);
    fun3(x, x);
    fun4(x, x);
    fun5(x, y);
    fun6(x, x);
    fun7(x, x);

    return -1;
}
