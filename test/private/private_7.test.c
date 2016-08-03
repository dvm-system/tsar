int main() {
  int I, X, Y;
  #pragma sapfor analysis dependency(I, X)
  for (I = 0; I < 10; ++I) {
    if (I == 0)
      X = X + 1;
  }
  return 0;
}