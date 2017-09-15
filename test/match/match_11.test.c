#define MAIN int I;  \
#pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>) expansion(match_11.c:3:14) \
for (I = 0; I < 10; ++I); return 0;

int main() { MAIN }