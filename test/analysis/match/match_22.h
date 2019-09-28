  for (I = 0; I < 10; ++I) {}
  I = 0;
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end: 