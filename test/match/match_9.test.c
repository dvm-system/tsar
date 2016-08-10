#define LOOP \
  #pragma sapfor analysis dependency(I) expansion(match_9.c:7:3) \
  for (I = 0; I < 10; ++I); \
  #pragma sapfor analysis dependency(J) expansion(match_9.c:7:3) \
  for (J = 0; J < 10; ++J);

int main() {
  int I, J;
  LOOP
  return 0;
}