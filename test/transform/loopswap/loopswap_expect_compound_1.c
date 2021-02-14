int f() {
	int c = 0;
#pragma spf transform swaploops
int k = 5;
{
	for (int i = 0; i < 10; i++)
		c += i;
}
	return c;
}
//CHECK: Error while processing loopswap_expect_compound_1.
//CHECK: loopswap_expect_compound_1.c:3:23: error: expected compound statement after pragma
//CHECK: #pragma spf transform swaploops
//CHECK:                       ^
//CHECK: 1 error generated.
//CHECK: Error while processing loopswap_expect_compound_1.c.
