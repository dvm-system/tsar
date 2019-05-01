float A[10][20];
float *P;

int foo(int I, int J) {

#pragma spf assert nomacro
  {
    int X = I;
    int Y = J;
    int Z = 0;
    A[I][J] = I + J + 0;
    Z = 1;
    *(1 + P + I + J) = 1;
    return ((X = I + 1) > 0) ? X : 1;
  }
}
