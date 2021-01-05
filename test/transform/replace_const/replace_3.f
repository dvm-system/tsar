      subroutine bar(integer x)
        x = 5
      end

      module baz
        integer k
        parameter(k = 4)
        integer a(10, 2:5)
        contains
          subroutine foo(integer i)
            call bar(a(i, 3))
            a(i,3_k) = a(i + (-1), i2_0) + a(k,4)
          end
        end
      end
!CHECK: 
