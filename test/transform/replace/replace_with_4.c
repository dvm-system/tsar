
void bar(int *Y) { *Y = 2 * *Y; }

void foo(int *X) {
#pragma spf metadata replace(bar(X))
   *X = *X + *X;
}

int baz() {
  int Z = 5;
#pragma spf transform replace with(foo)
  bar(&Z);
  return Z;
}
//CHECK: 
