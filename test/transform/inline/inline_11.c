void f(int *X) { *X = 10; }

#define INLINE _Pragma("spf transform inline")

void g() {
  int Y;
  INLINE
  f(&Y);
}
//CHECK: inline_11.c:7:3: warning: unable to remove directive in macro
//CHECK:   INLINE
//CHECK:   ^
//CHECK: inline_11.c:3:16: note: expanded from macro 'INLINE'
//CHECK: #define INLINE _Pragma("spf transform inline")
//CHECK:                ^
//CHECK: <scratch space>:2:16: note: expanded from here
//CHECK:  spf transform inline
//CHECK:                ^
//CHECK: 1 warning generated.
