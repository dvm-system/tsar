int main() {
  int I, X, Y;
  #pragma sapfor analysis firstprivate(<X,4>) dynamicprivate(<X,4>) dependency(<I,4>) shared(<Y,4>) explicitaccess(<I,4>, <X,4>, <Y,4>)
  for (I = 0; I < 10; ++I)
    if (Y > 0)
      X = I;
  Y = X;
  return 0;
}