int main() {
  int I, X, Y;
  #pragma sapfor analysis dependency(<I,4>, <X,4>) shared(<Y,4>) explicitaccess(<I,4>, <X,4>, <Y,4>)
  for (I = 0; I < 10; ++I) {
    if (Y > 0)
      X = I;
    X = X + 1;
  }
  return 0;
}