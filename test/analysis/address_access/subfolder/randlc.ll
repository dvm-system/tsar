; ModuleID = 'randlc.c'
source_filename = "randlc.c"
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.15.0"

@g = common global double* null, align 8

; Function Attrs: noinline nounwind optnone ssp uwtable
define double @randlc(double*, double) #0 {
  %3 = alloca double*, align 8
  %4 = alloca double, align 8
  %5 = alloca double, align 8
  %6 = alloca double, align 8
  %7 = alloca double, align 8
  %8 = alloca double, align 8
  %9 = alloca double, align 8
  %10 = alloca double, align 8
  %11 = alloca double, align 8
  %12 = alloca double, align 8
  %13 = alloca double, align 8
  %14 = alloca double, align 8
  %15 = alloca double, align 8
  %16 = alloca double, align 8
  %17 = alloca double, align 8
  %18 = alloca double, align 8
  store double* %0, double** %3, align 8
  store double %1, double* %4, align 8
  store double 0x3E80000000000000, double* %5, align 8
  store double 0x3D10000000000000, double* %6, align 8
  store double 0x4160000000000000, double* %7, align 8
  store double 0x42D0000000000000, double* %8, align 8
  %19 = load double, double* %4, align 8
  %20 = fmul double 0x3E80000000000000, %19
  store double %20, double* %9, align 8
  %21 = load double, double* %9, align 8
  %22 = fptosi double %21 to i32
  %23 = sitofp i32 %22 to double
  store double %23, double* %13, align 8
  %24 = load double, double* %4, align 8
  %25 = load double, double* %13, align 8
  %26 = fmul double 0x4160000000000000, %25
  %27 = fsub double %24, %26
  store double %27, double* %14, align 8
  %28 = load double*, double** %3, align 8
  %29 = load double, double* %28, align 8
  %30 = fmul double 0x3E80000000000000, %29
  store double %30, double* %9, align 8
  %31 = load double, double* %9, align 8
  %32 = fptosi double %31 to i32
  %33 = sitofp i32 %32 to double
  store double %33, double* %15, align 8
  %34 = load double*, double** %3, align 8
  %35 = load double, double* %34, align 8
  %36 = load double, double* %15, align 8
  %37 = fmul double 0x4160000000000000, %36
  %38 = fsub double %35, %37
  store double %38, double* %16, align 8
  %39 = load double, double* %13, align 8
  %40 = load double, double* %16, align 8
  %41 = fmul double %39, %40
  %42 = load double, double* %14, align 8
  %43 = load double, double* %15, align 8
  %44 = fmul double %42, %43
  %45 = fadd double %41, %44
  store double %45, double* %9, align 8
  %46 = load double, double* %9, align 8
  %47 = fmul double 0x3E80000000000000, %46
  %48 = fptosi double %47 to i32
  %49 = sitofp i32 %48 to double
  store double %49, double* %10, align 8
  %50 = load double, double* %9, align 8
  %51 = load double, double* %10, align 8
  %52 = fmul double 0x4160000000000000, %51
  %53 = fsub double %50, %52
  store double %53, double* %17, align 8
  %54 = load double, double* %17, align 8
  %55 = fmul double 0x4160000000000000, %54
  %56 = load double, double* %14, align 8
  %57 = load double, double* %16, align 8
  %58 = fmul double %56, %57
  %59 = fadd double %55, %58
  store double %59, double* %11, align 8
  %60 = load double, double* %11, align 8
  %61 = fmul double 0x3D10000000000000, %60
  %62 = fptosi double %61 to i32
  %63 = sitofp i32 %62 to double
  store double %63, double* %12, align 8
  %64 = load double, double* %11, align 8
  %65 = load double, double* %12, align 8
  %66 = fmul double 0x42D0000000000000, %65
  %67 = fsub double %64, %66
  %68 = load double*, double** %3, align 8
  store double %67, double* %68, align 8
  %69 = load double*, double** %3, align 8
  %70 = load double, double* %69, align 8
  %71 = fmul double 0x3D10000000000000, %70
  store double %71, double* %18, align 8
  %72 = load double, double* %18, align 8
  ret double %72
}

attributes #0 = { noinline nounwind optnone ssp uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "darwin-stkchk-strong-link" "disable-tail-calls"="false" "frame-pointer"="all" "less-precise-fpmad"="false" "min-legal-vector-width"="0" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "probe-stack"="___chkstk_darwin" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.module.flags = !{!0, !1, !2}
!llvm.ident = !{!3}

!0 = !{i32 2, !"SDK Version", [3 x i32] [i32 10, i32 15, i32 4]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 7, !"PIC Level", i32 2}
!3 = !{!"Apple clang version 11.0.3 (clang-1103.0.32.62)"}
