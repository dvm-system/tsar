int f2() { return 2; }
int f1() { 
#pragma spf transform inline
  return 1 + (0 || f2());
}
//CHECK: inline_8.c:4:20: warning: disable inline expansion in right hand side of logical operator
//CHECK:   return 1 + (0 || f2());
//CHECK:                    ^
//CHECK: 1 warning generated.
