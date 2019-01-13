int foo() {
   int X = 45;
   return X;
}

int foo_1() {
   int X = 9;
   return X;
}

void bar() {
  int X = 0;
#pragma spf transform inline
  X = foo() + foo_1();
}

int main(){
  int X = 0;
#pragma spf transform inline
{
  X = foo() + foo_1();
//  bar();
}
  return 0;
}
