void bar(int *X) { *X = 5; }
int main() {
  int S = 0;
#pragma omp parallel for default(shared) reduction(+ : S)
  for (int I = 0; I < 10; ++I) {
    int X;
    bar(&X);
    S += X;
  }
  return S;
}