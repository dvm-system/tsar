//
// Created by Vladislav Volodkin on 10/18/20.
//
#include <stdlib.h>

int** glob;

void fun1(int *a, int *b, int **ptra) {
  int **ptr;
  ptr = (int**) malloc(sizeof(int*));
  glob = ptr;

  int *c = &(a[3]);
  *ptr = c;
}