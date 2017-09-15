int main() {
  int I;
  #pragma sapfor analysis dependency(<I,4>) private(<X,4>) explicitaccess(<I,4>, <X,4>)
  for (I = 0; I < 10; ++I) {
    int X = I;   
  }
}
