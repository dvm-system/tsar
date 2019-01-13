void bar() { }

void foo() {
  #include "inline_macro_28.h"
}

//CHECK: In file included from inline_macro_28.c:4:
//CHECK: inline_macro_28.h:1:23: warning: unable to remove directive in include
//CHECK: #pragma spf transform inline
//CHECK:                       ^
//CHECK: inline_macro_28.h:2:1: warning: disable inline expansion in header file
//CHECK: bar();
//CHECK: ^
//CHECK: 2 warnings generated.
