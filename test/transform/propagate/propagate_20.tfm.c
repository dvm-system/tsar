
float foo(int N, float (*A)[N]) {

  int X;
  X = N - 1;
  return (A[(N - 1)][(N - 1) - 1] = 2.1);
}
