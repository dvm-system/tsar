int f1() { return 0;}

void f(int X) {
  #pragma spf transform inline
  {
      switch(X) {
        case 1: {f1();} break;
        case 2: f1(); break;
        default: f1(); break;
      }
  }  
}  

     
//CHECK: 
