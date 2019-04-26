struct S {
  enum {
    AA = 1,
    SIZE = 10
  };
};

double A[SIZE][2][SIZE][SIZE];

double foo(int I, int J) {
#pragma spf transform propagate
  double (*B)[SIZE] = A[I][AA];
  return B[1][J];
  }
//CHECK: 
