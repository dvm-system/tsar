#include "inline_merge_1.h"

void foo() {
  Global = 5;

  /* bar() is inlined below */
#pragma spf assert nomacro
  { Global = 10; }
}
