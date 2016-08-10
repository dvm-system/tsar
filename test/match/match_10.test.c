#define INIT I = 0

int main() {
  int I, J;
  #pragma sapfor analysis dependency(I)
  for (INIT; I < 10; ++I);
  return 0;
}