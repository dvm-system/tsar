#define F(f_) void f_() {}

F(f1) 

void f2() {
#pragma spf transform inline
  f1();
}
//CHECK: inline_macro_5.c:3:3: warning: disable inline expansion
//CHECK: F(f1) 
//CHECK:   ^
//CHECK: inline_macro_5.c:3:1: note: macro prevent inlining
//CHECK: F(f1) 
//CHECK: ^
//CHECK: inline_macro_5.c:1:26: note: expanded from macro 'F'
//CHECK: #define F(f_) void f_() {}
//CHECK:                          ^
//CHECK: 1 warning generated.
