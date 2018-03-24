void foo() {
  int X;
  #pragma sapfor analysis dependency(<I,4>) shared(<X,4>) explicitaccess(<I,4>, <X,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (int I = 0; I < X; ++I);
}