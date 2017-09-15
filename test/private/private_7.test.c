int main() {
  int I, X, Y;
  #pragma sapfor analysis dependency(<I,4>, <X,4>) explicitaccess(<I,4>, <X,4>)
  for (I = 0; I < 10; ++I) {
    if (I == 0)
      X = X + 1;
  }
  return 0;
}