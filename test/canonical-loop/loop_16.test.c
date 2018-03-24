void foo() {
  int X = 1;
  #pragma sapfor analysis dependency(<I,8>) shared(<X,4>) explicitaccess(<I,8>, <X,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (long long I = 10; I > 0; I = I - X);
}