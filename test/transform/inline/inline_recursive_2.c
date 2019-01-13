int f1();

int f2() {
  return f1();
}

int f1() {
#pragma spf transform inline
  return f2() + 1;
}


//CHECK: 
