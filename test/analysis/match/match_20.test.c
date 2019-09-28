#define LOOP int I;  \
#pragma sapfor analysis unavailable expansion(match_20.c:4:3) \
for (I = 0; I < 10; ++I) return 0;

int main() {
  LOOP
}