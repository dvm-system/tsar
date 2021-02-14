int f() {
	int s = 0;
#pragma spf transform swaploops
{
	s = 15;
	for (int i = 0; i < 10; i++) {
		s += i;
	}
	for (int k = 0; k < 15; k++) {
		s += k;
	}
}
	return s;
}
//CHECK: Error while processing loopswap_redundant_stmt_1.
//CHECK: loopswap_redundant_stmt_1.c:5:2: error: pragma should only contain loops or other pragma
//CHECK:         s = 15;
//CHECK:         ^
//CHECK: 1 error generated.
//CHECK: Error while processing loopswap_redundant_stmt_1.c.
