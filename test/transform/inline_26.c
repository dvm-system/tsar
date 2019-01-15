float mAbs(float Val) { return Val < 0 ? Val * -1 : Val; }

int main() {
  float i = 0.0;
#pragma spf transform inline
  i = mAbs((float)-435);
  return 0;
}
//CHECK: 
