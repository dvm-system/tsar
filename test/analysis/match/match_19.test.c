#define LOOP \
int I, J, K;  \
#pragma sapfor analysis unavailable expansion(match_19.c:14:3) \
\
for (I = 0; I < 10; ++I); \
J = 0; \
head: \
 if (J < 10) \
   goto end; \
 J = J + 1; \
 goto head; \
end:  \
#pragma sapfor analysis unavailable expansion(match_19.c:14:3) \
\
for (K = 0; K < 10; ++K);   
    
int main() {
  LOOP
  return 0;
}                         