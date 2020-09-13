int* fun3(int *a, int *b) {  // Function [fun3]: 	b,
    int *c = b;
    return c;
}



//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [fun3]: [ADDRESS-ACCESS] 	b,

