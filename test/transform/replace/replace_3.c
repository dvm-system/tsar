#include "replace_3_data.h"
#include "replace_3_struct.h"
#include "replace_3.h"

//CHECK: In file included from replace_3.c:3:
//CHECK: replace_3.h:2:25: warning: unable to remove directive in include
//CHECK:   #pragma spf transform replace (S)
//CHECK:                         ^
//CHECK: 1 warning generated.
