#define LOOP \
  for (I = 0; I < 10; ++I); \
  for (J = 0; J < 10; ++J);

int main() {
  int I, J;
  LOOP
  return 0;
}