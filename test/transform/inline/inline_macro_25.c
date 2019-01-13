int f() {
#include "inline_macro_25.h"
}

void f1() {
#pragma spf transform inline
  f();
}
//CHECK: inline_macro_25.c:1:5: warning: disable inline expansion
//CHECK: int f() {
//CHECK:     ^
//CHECK: inline_macro_25.c:2:1: note: macro prevent inlining
//CHECK: #include "inline_macro_25.h"
//CHECK: ^
//CHECK: 1 warning generated.
