void foo() {
  long long X = 1;
  #pragma sapfor analysis dependency(<I,4>) shared(<X,8>) explicitaccess(<I,4>, <X,8>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (int I = 10; I > 0; I = I - X);
}