int f() {
	int k;
#pragma spf transform swaploops
{
	for (int i = 0; i < 10; ++i)
		k = i;
	for (int j = 0; j < 15; ++j)
		k = j;
}
	return k * 2;
}

//CHECK: loopswap_output_1.c:3:9: warning: unable to swap loops due to the output dependence of variable 'k' declared at <loopswap_output_1.c:2:2, col:6>
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
