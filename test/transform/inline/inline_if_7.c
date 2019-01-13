int foo() { return 221; }

int bar(int Val) {
#pragma spf transform inline
{
  if (Val)
    return foo() + Val;
}
  return Val;
}

int main() {
#pragma spf transform inline
  return bar(13);
}
//CHECK: 
