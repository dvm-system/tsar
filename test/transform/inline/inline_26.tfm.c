float mAbs(float Val) { return Val < 0 ? Val * -1 : Val; }

int main() {
  float i = 0.0;

  /* mAbs((float)-435) is inlined below */
  float R0;
#pragma spf assert nomacro
  {
    float Val0 = (float)-435;
    R0 = Val0 < 0 ? Val0 * -1 : Val0;
  }
  i = R0;
  return 0;
}
