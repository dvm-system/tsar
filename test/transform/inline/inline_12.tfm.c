#include <stdio.h>

void g(int ArgN, char *Val) { printf("Arg=%d Val=%s\n", ArgN, Val); }

int main(int argc, char **argv) {

  /* g(argc - 1, argv[argc - 1]) is inlined below */
#pragma spf assert nomacro
  {
    int ArgN0 = argc - 1;
    char *Val0 = argv[argc - 1];
    printf("Arg=%d Val=%s\n", ArgN0, Val0);
  }

  return 0;
}
