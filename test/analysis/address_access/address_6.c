int **addr = (int **) 11231231;
int **getAddr() {
    return addr;
}

void fun6(int *a, int *b) {  // Function [fun6]: 	b,
    int **addr = getAddr();
    *addr = b;
}

//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [getAddr]: 
//CHECK: [ADDRESS-ACCESS] Function [fun6]: [ADDRESS-ACCESS] 	b,
