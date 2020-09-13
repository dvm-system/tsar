int * glob;

void bar(int *X) {
    *X = 22;
    glob = X;
}

int main() {
  int S = 0;
  for (int I = 0; I < 1000000; ++I) {
    int X = I;
    bar(&X);
    S += X;
  }
  return S;
}