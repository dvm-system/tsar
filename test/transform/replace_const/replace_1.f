      function foo()
        integer foo
        real a(0:9)
        do i = 1, 10
          a(i - 1) = i
        enddo
        foo = 0
        do 1 i = 0, 8
          foo = foo + a(i)
          a(i + 1) = a(i + 1) + a(i)
1       continue
        foo = foo + a(9)
      end
!CHECK: 
