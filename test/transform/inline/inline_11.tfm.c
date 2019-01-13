void f(int *X) { *X = 10; }

#define INLINE _Pragma("spf transform inline")

void g() {
  int Y;
  INLINE
  /* f(&Y) is inlined below */
#pragma spf assert nomacro
  {
    int *X0 = &Y;
    *X0 = 10;
  }
}
