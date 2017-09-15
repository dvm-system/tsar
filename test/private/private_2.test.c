int main() {
  int I, X;
  #pragma sapfor analysis firstprivate(<X,4>) secondtolastprivate(<X,4>) dependency(<I,4>) explicitaccess(<I,4>, <X,4>)
  for (I = 0; I < 10; ++I)
    X = I;
  X = 2 * X;
  return 0;
}