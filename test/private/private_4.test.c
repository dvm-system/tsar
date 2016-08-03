int main() {
  int I, X, Y;
  #pragma sapfor analysis firstprivate(X) dynamicprivate(X) dependency(I) shared(Y)
  for (I = 0; I < 10; ++I)
    if (Y > 0)
      X = I;
  Y = X;
  return 0;
}