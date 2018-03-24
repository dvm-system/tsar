void foo() {
  int I, J;
  #pragma sapfor analysis dependency(<I,4>, <J,4>) explicitaccess(<I,4>, <J,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis non-canonical
  for (I = 0; I < J; ++I)
    ++J;
}