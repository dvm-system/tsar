int M(){
  return 78;
}

int M1(){
  return 79;
}

int F(int X) {
  #pragma spf transform inline 
  return X + M() + M1();

}


int main()
{
  int x = 0;

  #pragma spf transform inline 
  x += F(M() + M1());
  return 0;
}
