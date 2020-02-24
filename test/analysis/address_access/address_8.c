int *glob;

void fun8(int *a, int *b) {  // Function [fun8]: 	a,
    b[1] = b[2];
    int **c = &a;
    glob = *c;
}

//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [fun8]: [ADDRESS-ACCESS] 	a,

