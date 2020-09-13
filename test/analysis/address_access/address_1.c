int *glob;

void fun1(int *a, int *b) {  // Function [fun1]: 	a,
    b[11] = b[12];
    glob = a;
}

//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [fun1]: [ADDRESS-ACCESS] 	a,
