void foo() {
  int I, *X = &I;
  for (I = 0; I < *X ; ++I);
}