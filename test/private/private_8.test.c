int main() {
  int I, K, N;
  #pragma sapfor analysis dependency(I) private(K, N)
  for (I = 0; I < 10; ++I) {
    K = I;
    N = K;
  }
}