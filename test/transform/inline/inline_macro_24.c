#include "inline_macro_24.h"
//CHECK: In file included from inline_macro_24.c:1:
//CHECK: inline_macro_24.h:4:25: warning: unable to remove directive in include
//CHECK:   #pragma spf transform inline
//CHECK:                         ^
//CHECK: inline_macro_24.h:5:3: warning: disable inline expansion in header file
//CHECK:   f();
//CHECK:   ^
//CHECK: 2 warnings generated.
