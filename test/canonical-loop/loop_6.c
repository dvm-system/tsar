void foo() {
  int I, *X = &I;
  for (I = 0; I < 10; I = I + *X);
}
