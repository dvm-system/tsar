int main() {
  int I, X;
  I = 0;
  #pragma sapfor analysis firstprivate(<X,4>) secondtolastprivate(<X,4>) dependency(<I,4>) explicitaccess(<I,4>, <X,4>)
  while (I < 10) {
    X = I;
    ++I;
  };
  X = X + 1;
  return 0;
}