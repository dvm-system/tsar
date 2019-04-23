char foo(char X, char Y) {
#pragma spf transform propagate
  char C = X;
  return C && (C = X > 0 ? X : X + 1) && C;
}
//CHECK: 
