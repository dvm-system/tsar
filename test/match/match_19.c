#define LOOP \
int I, J, K; \
for (I = 0; I < 10; ++I); \
J = 0; \
head: \
 if (J < 10) \
   goto end; \
 J = J + 1; \
 goto head; \
end: \
for (K = 0; K < 10; ++K);   
    
int main() {
  LOOP
  return 0;
}                         