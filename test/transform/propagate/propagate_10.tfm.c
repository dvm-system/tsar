int A[10];

void foo(int I) {

#pragma spf assert nomacro
  {
    int X = I;
    A[++X] = I;
  }
}
