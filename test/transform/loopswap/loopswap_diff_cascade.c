int f() {
	int k = 7;
#pragma spf transform swaploops
{
	for (int n = 1; n < 10; n++) {
		k *= 15;
	}
	#pragma spf transform swaploops
	{
		for (int i = 0; i < 10; i++)
			k += (i + 1) * 4;
		for (int j = 3; j < 7; j++)
			k -= j * 12;
	}
	for (int m = 4; m < 15; m++) {
		k *= m + 1;
	}
	#pragma spf transform swaploops
	{
		for (int i = 0; i < 10; i++)
			k += (i + 1) * 4;
		for (int j = 3; j < 7; j++)
			k -= j * 12;
	}
}
	return k;
}
//CHECK: 
