struct STy { int X; };

#define CALL(X) return bar(X);

int bar(int X) { return X * 2; }
int bar_new(int X) {
#pragma spf metadata replace(bar(X))
  return X + X;
}

int foo (int X) {
 #pragma spf transform replace with(bar_new)
 CALL(X)
}

//CHECK: replace_macro_8.c:11:5: warning: disable structure replacement
//CHECK: int foo (int X) {
//CHECK:     ^
//CHECK: replace_macro_8.c:13:2: note: macro prevent replacement
//CHECK:  CALL(X)
//CHECK:  ^
//CHECK: replace_macro_8.c:3:24: note: expanded from macro 'CALL'
//CHECK: #define CALL(X) return bar(X);
//CHECK:                        ^
//CHECK: 1 warning generated.
