#include "inline_merge_1.h"

void foo() {
  Global = 5;
#pragma spf transform inline
  bar();
}
//CHECK: 
//CHECK-1: 
