int A[10];

void foo(int I) {

  {
    int X = I;
    A[++X] = I;
  }
}
