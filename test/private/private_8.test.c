int main() {
  int I, K, N;
  #pragma sapfor analysis dependency(<I,4>) private(<K,4>, <N,4>) explicitaccess(<I,4>, <K,4>, <N,4>)
  for (I = 0; I < 10; ++I) {
    K = I;
    N = K;
  }
}