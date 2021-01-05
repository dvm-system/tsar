      function foo()
        integer foo
        real a(0:9)
      integer i1_0, i9_0
      i1_0 = 1
      i9_0 = 9
        do i = 1, 10
          a(i - i1_0) = i
        enddo
        foo = 0
        do 1 i = 0, 8
          foo = foo + a(i)
          a(i + i1_0) = a(i + i1_0) + a(i)
1       continue
        foo = foo + a(i9_0)
      end
