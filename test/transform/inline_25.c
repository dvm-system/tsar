#include <stdio.h>

int main()
{
	#pragma spf transform inline
	printf("hello world\n");
	return 0;
}
//CHECK: In included file:
//CHECK: warning: disable inline expansion of non-user defined function
