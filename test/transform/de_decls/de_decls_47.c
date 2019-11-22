int foo() {
  int I;
  int X = 5;
  for (I = 0, X = 5;;);
  return X;
}
//CHECK: 
