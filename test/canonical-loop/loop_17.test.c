void foo() {
  unsigned X = 1;
  #pragma sapfor analysis dependency(<I,4>) shared(<X,4>) explicitaccess(<I,4>, <X,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (long I = 0; 10 > I; I = I + X);
}