void foo() {
  int I, *X = &I;
  #pragma sapfor analysis dependency(<I,4>, <X,4>) shared(<X,8>) explicitaccess(<I,4>, <X,4>, <X,8>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis non-canonical
  for (I = 0; I < *X ; ++I);
}