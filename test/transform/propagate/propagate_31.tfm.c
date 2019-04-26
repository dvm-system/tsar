struct S {
  enum { AA = 1, SIZE = 10 };
};

double A[SIZE][2][SIZE][SIZE];

double foo(int I, int J) {

  double(*B)[SIZE] = A[I][AA];
  return (A[I][AA])[1][J];
}
