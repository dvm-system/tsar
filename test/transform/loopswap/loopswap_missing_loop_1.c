int f() {
	int s = 0;
#pragma spf transform swaploops
{
	for (int i = 1; i < 10; i++) {
		s += i * i;
	}
}
	return s;
}
//CHECK: loopswap_missing_loop_1.c:3:9: warning: not enough loops for swapping
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
