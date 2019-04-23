char foo(char X, char Y) {
#pragma spf transform propagate
  char C = Y;
  return (C = X);
}
//CHECK: 
