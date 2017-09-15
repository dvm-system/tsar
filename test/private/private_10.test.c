int main() {
  int I, X;
  I = 0;
  #pragma sapfor analysis lastprivate(<X,4>) dependency(<I,4>) explicitaccess(<I,4>, <X,4>)
  do {
    X = I;
    ++I;
  } while (I < 10);
  X = X + 1;
  return 0;
}