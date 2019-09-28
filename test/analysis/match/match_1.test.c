int main() {
  int I;
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  for (I = 0; I < 10; ++I);
  return 0;
}