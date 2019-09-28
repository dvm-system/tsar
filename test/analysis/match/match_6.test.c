int main() {
  int I, J;
  #pragma sapfor analysis dependency(<I,4>) private(<J,4>) explicitaccess(<I,4>, <J,4>)
  for (I = 0; I < 10; ++I) {
    J  = 0;
    #pragma sapfor analysis dependency(<J,4>) explicitaccess(<J,4>)
    for (; J < 10; ++J);
  }
  return 0;
}