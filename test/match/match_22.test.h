  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>) include(match_22.c:3:12)
  for (I = 0; I < 10; ++I) {}
  I = 0;
#pragma sapfor analysis loop(implicit) include(match_22.c:3:12)
#pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>) include(match_22.c:3:12)
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end: 