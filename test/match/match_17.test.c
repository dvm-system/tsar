#define LOOP  \
#pragma sapfor analysis dependency(I) expansion(match_17.c:6:3) \
 \
#pragma sapfor analysis dependency(I) expansion(match_17.c:5:3) \
for (I = 0; I < 10; ++I);

int main() {
  int I;
  LOOP
  LOOP
  return 0;
}