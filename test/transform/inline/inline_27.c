#include <stdlib.h>
#include <stdio.h>

int * foo1(int N, int **A) {
  *A = (int *) malloc(N * sizeof(int));
  for (int I = 0; I < N; ++I)
    (*A)[I] = I;
  return *A;
}

int foo2(int N, int *A) {
  int S = 0;
  for (int I = 0; I < N; ++I)
    S = S + A[I];
  return S;	  
 }


int main(){
	int *A, N, S;
#pragma spf transform inline
{
  S = foo2(N, foo1(N, &A));
}
  printf("S=%d\n", S);
  return 0;
}
//CHECK: 
