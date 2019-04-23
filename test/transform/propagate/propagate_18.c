char bar();

char foo() {
#pragma spf transform propagate
  char C;
  return (C = bar()) && C;
}
//CHECK: 
