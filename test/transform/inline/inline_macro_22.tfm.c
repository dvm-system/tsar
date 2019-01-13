// This test contains macro in a location which is omitted in the AST.

#define C :

void f() { L C return; }

void f1() { f(); }
