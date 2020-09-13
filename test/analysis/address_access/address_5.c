void fun5(int *a, int **b) {  // Function [fun5]: 	a,
    int *c = &(a[10]);
    *b = c;
}



//CHECK: Printing analysis 'address-access':
//CHECK: [ADDRESS-ACCESS] Function [fun5]: [ADDRESS-ACCESS] 	a,

