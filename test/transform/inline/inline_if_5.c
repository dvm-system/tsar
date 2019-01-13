int f() { return 1; }

int g(int X) {
#pragma spf transform inline
  if (X && f())
    return X;
  return 0;
}
//CHECK: inline_if_5.c:5:12: warning: disable inline expansion in right hand side of logical operator
//CHECK:   if (X && f())
//CHECK:            ^
//CHECK: 1 warning generated.
