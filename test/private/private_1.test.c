int main() {
  int I, X;
  #pragma sapfor analysis dependency(I) private(X)
  for (I = 0; I < 10; ++I)
    X = I;
  return 0;
}