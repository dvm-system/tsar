int M(int X) {
  return X;
}

int main()
{
  int X = 0;

  #pragma spf transform inline 
  {
  if (1)
    X = M(78) + X;
  }
  return 0;
}
