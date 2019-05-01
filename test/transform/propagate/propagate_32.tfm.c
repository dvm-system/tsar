enum { N = 4, M = 20 };

struct {
  float A[N][M][5];
} S;

void foo() {

#pragma spf assert nomacro
  {
    float(*A)[M][5] = S.A;
    float(*B)[5] = S.A[2];
    float(*C) = S.A[2][3];
    for (int K = 0; K < N; ++K)
      for (int I = 0; I < M; ++I)
        for (int J = 0; J < 5; ++J)
          S.A[K][I][J] = K + I + J;
    for (int I = 0; I < M; ++I)
      for (int J = 0; J < 5; ++J)
        S.A[2][I][J] = S.A[2][I][J] + I + J;
    for (int J = 0; J < 5; ++J)
      S.A[2][3][J] = S.A[2][3][J] + J;
  }
}
