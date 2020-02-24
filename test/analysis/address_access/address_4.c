int *glob;

void fun4(int *a, int *b) {  // Function [fun4]: 	a,
    int *d = &(b[10]);
    int *c = &(a[10]);
    glob = c;
}



//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [fun4]: [ADDRESS-ACCESS] 	a,

