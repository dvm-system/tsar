//-------------------------------------------------------------------------//
//                                                                         //
//  This benchmark is a serial C version of the NPB EP code. This C        //
//  version is developed by the Center for Manycore Programming at Seoul   //
//  National University and derived from the serial Fortran versions in    //
//  "NPB3.3-SER" developed by NAS.                                         //
//                                                                         //
//  Permission to use, copy, distribute and modify this software for any   //
//  purpose with or without fee is hereby granted. This software is        //
//  provided "as is" without express or implied warranty.                  //
//                                                                         //
//  Information on NPB 3.3, including the technical report, the original   //
//  specifications, source code, results and information on how to submit  //
//  new results, is available at:                                          //
//                                                                         //
//           http://www.nas.nasa.gov/Software/NPB/                         //
//                                                                         //
//  Send comments or suggestions for this C version to cmp@aces.snu.ac.kr  //
//                                                                         //
//          Center for Manycore Programming                                //
//          School of Computer Science and Engineering                     //
//          Seoul National University                                      //
//          Seoul 151-744, Korea                                           //
//                                                                         //
//          E-mail:  cmp@aces.snu.ac.kr                                    //
//                                                                         //
//-------------------------------------------------------------------------//

//-------------------------------------------------------------------------//
// Authors: Sangmin Seo, Jungwon Kim, Jun Lee, Jeongho Nah, Gangwon Jo,    //
//          and Jaejin Lee                                                 //
//-------------------------------------------------------------------------//

//--------------------------------------------------------------------
//      program EMBAR
//--------------------------------------------------------------------
//  This is the serial version of the APP Benchmark 1,
//  the "embarassingly parallel" benchmark.
//
//
//  M is the Log_2 of the number of complex pairs of uniform (0, 1) random
//  numbers.  MK is the Log_2 of the size of each batch of uniform random
//  numbers.  MK can be set for convenience on a given system, since it does
//  not affect the results.
//--------------------------------------------------------------------

#define _CRT_SECURE_NO_DEPRECATE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#undef max
#undef min

#include "npbparams.h"
#include "type.h"
#include "print_results.h"
#include "randdp.h"
#include "timers.h"

#include <time.h>
#ifndef DOS
#ifndef _WIN32
#include <sys/time.h>
#else
#include <sys/timeb.h>
#endif
#endif

#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))

#if 1
// For TSAR.
enum { MK = 16, MM = (M - MK), NN = (1 << MM), NK = (1 << MK), NQ = 10 };
#else
// For CDVMH.
#define MK 16
#define MM (M - MK)
#define NN (1 << MM)
#define NK (1 << MK)
#define NQ 10
#endif

const double EPSILON = 1.0e-8;
const double A = 1220703125.0;
const double S = 271828183.0;

static double x[2 * NK];
static double q[NQ];

int main() {
  double Mops, t1, t2, t3, t4, x1, x2;
  double sx, sy, tm, an, tt, gc;
  double sx_verify_value, sy_verify_value, sx_err, sy_err;
  int np;
  int i, ik, kk, l, k, nit;
  int k_offset, j;
  logical verified, timers_enabled;

  double dum[3] = {1.0, 1.0, 1.0};
  char size[16];

  FILE *fp;

  if ((fp = fopen("timer.flag", "r")) == NULL) {
    timers_enabled = false;
  } else {
    timers_enabled = true;
    fclose(fp);
  }

  //--------------------------------------------------------------------
  //  Because the size of the problem is too large to store in a 32-bit
  //  integer for some classes, we put it into a string (for printing).
  //  Have to strip off the decimal point put in there by the floating
  //  point print statement (internal file)
  //--------------------------------------------------------------------

  sprintf(size, "%15.0lf", pow(2.0, M + 1));
  j = 14;
  if (size[j] == '.')
    j--;
  size[j + 1] = '\0';
  printf("\n\n NAS Parallel Benchmarks (NPB3.3-SER-C) - EP Benchmark\n");
  printf("\n Number of random numbers generated: %15s\n", size);

  verified = false;

  //--------------------------------------------------------------------
  //  Compute the number of "batches" of random number pairs generated
  //  per processor. Adjust if the number of processors does not evenly
  //  divide the total number
  //--------------------------------------------------------------------

  np = NN;

  //--------------------------------------------------------------------
  //  Call the random number generator functions and initialize
  //  the x-array to reduce the effects of paging on the timings.
  //  Also, call all mathematical functions that are used. Make
  //  sure these initializations cannot be eliminated as dead code.
  //--------------------------------------------------------------------

  {
/* vranlc(0, &dum[0], dum[1], &dum[2]) is inlined below */
#pragma spf assert nomacro
    {
      int n0 = 0;
      double *x0 = &dum[0];
      double a0 = dum[1];
      double *y2 = &dum[2];
      //--------------------------------------------------------------------
      //
      //  This routine generates N uniform pseudorandom double precision numbers
      //  in the range (0, 1) by using the linear congruential generator
      //
      //  x_{k+1} = a x_k  (mod 2^46)
      //
      //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
      //  numbers before repeating.  The argument A is the same as 'a' in the
      //  above formula, and X is the same as x_0.  A and X must be odd double
      //  precision integers in the range (1, 2^46).  The N results are placed
      //  in Y and are normalized to be between 0 and 1.  X is updated to
      //  contain the new seed, so that subsequent calls to VRANLC using the
      //  same arguments will generate a continuous sequence.  If N is zero,
      //  only initialization is performed, and the variables X, A and Y are
      //  ignored.
      //
      //  This routine is the standard version designed for scalar or RISC
      //  systems. However, it should produce the same results on any single
      //  processor computer with at least 48 mantissa bits in double precision
      //  floating point data.  On 64 bit systems, double precision should be
      //  disabled.
      //
      //--------------------------------------------------------------------

      // r23 = pow(0.5, 23.0);
      ////  pow(0.5, 23.0) = 1.1920928955078125e-07
      // r46 = r23 * r23;
      // t23 = pow(2.0, 23.0);
      ////  pow(2.0, 23.0) = 8.388608e+06
      // t46 = t23 * t23;

      const double r23 = 1.1920928955078125e-07;
      const double r46 = r23 * r23;
      const double t23 = 8.388608e+06;
      const double t46 = t23 * t23;

      double t1, t2, t3, t4, a1, a2, x1, x2, z;

      int i;

      //--------------------------------------------------------------------
      //  Break A into two parts such that A = 2^23 * A1 + A2.
      //--------------------------------------------------------------------
      t1 = r23 * a0;
      a1 = (int)t1;
      a2 = a0 - t23 * a1;

      //--------------------------------------------------------------------
      //  Generate N results.   This loop is not vectorizable.
      //--------------------------------------------------------------------
      for (i = 0; i < n0; i++) {
        //--------------------------------------------------------------------
        //  Break X into two parts such that X = 2^23 * X1 + X2, compute
        //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
        //  X = 2^23 * Z + A2 * X2  (mod 2^46).
        //--------------------------------------------------------------------
        t1 = r23 * (*x0);
        x1 = (int)t1;
        x2 = *x0 - t23 * x1;
        t1 = a1 * x2 + a2 * x1;
        t2 = (int)(r23 * t1);
        z = t1 - t23 * t2;
        t3 = t23 * z + a2 * x2;
        t4 = (int)(r46 * t3);
        *x0 = t3 - t46 * t4;
        y2[i] = r46 * (*x0);
      }
    }

    /* randlc(&dum[1], dum[2]) is inlined below */
    double R0;
#pragma spf assert nomacro
    {
      double *x3 = &dum[1];
      double a3 = dum[2];
      //--------------------------------------------------------------------
      //
      //  This routine returns a uniform pseudorandom double precision number in
      //  the range (0, 1) by using the linear congruential generator
      //
      //  x_{k+1} = a x_k  (mod 2^46)
      //
      //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
      //  numbers before repeating.  The argument A is the same as 'a' in the
      //  above formula, and X is the same as x_0.  A and X must be odd double
      //  precision integers in the range (1, 2^46).  The returned value RANDLC
      //  is normalized to be between 0 and 1, i.e. RANDLC = 2^(-46) * x_1.  X
      //  is updated to contain the new seed x_1, so that subsequent calls to
      //  RANDLC using the same arguments will generate a continuous sequence.
      //
      //  This routine should produce the same results on any computer with at
      //  least 48 mantissa bits in double precision floating point data.  On 64
      //  bit systems, double precision should be disabled.
      //
      //  David H. Bailey     October 26, 1990
      //
      //--------------------------------------------------------------------

      // r23 = pow(0.5, 23.0);
      ////  pow(0.5, 23.0) = 1.1920928955078125e-07
      // r46 = r23 * r23;
      // t23 = pow(2.0, 23.0);
      ////  pow(2.0, 23.0) = 8.388608e+06
      // t46 = t23 * t23;

      const double r23 = 1.1920928955078125e-07;
      const double r46 = r23 * r23;
      const double t23 = 8.388608e+06;
      const double t46 = t23 * t23;

      double t1, t2, t3, t4, a1, a2, x1, x2, z;
      double r;

      //--------------------------------------------------------------------
      //  Break A into two parts such that A = 2^23 * A1 + A2.
      //--------------------------------------------------------------------
      t1 = r23 * a3;
      a1 = (int)t1;
      a2 = a3 - t23 * a1;

      //--------------------------------------------------------------------
      //  Break X into two parts such that X = 2^23 * X1 + X2, compute
      //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
      //  X = 2^23 * Z + A2 * X2  (mod 2^46).
      //--------------------------------------------------------------------
      t1 = r23 * (*x3);
      x1 = (int)t1;
      x2 = *x3 - t23 * x1;
      t1 = a1 * x2 + a2 * x1;
      t2 = (int)(r23 * t1);
      z = t1 - t23 * t2;
      t3 = t23 * z + a2 * x2;
      t4 = (int)(r46 * t3);
      *x3 = t3 - t46 * t4;
      r = r46 * (*x3);

      R0 = r;
    }
    dum[0] = R0;
#pragma omp parallel for default(shared)
    for (i = 0; i < 2 * NK; i++) {
      x[i] = -1.0e99;
    }
    Mops = log(sqrt(fabs(MAX(1.0, 1.0))));

    timer_clear(0);
    timer_clear(1);
    timer_clear(2);
    timer_start(0);

    t1 = A;
    double *x4 = &t1;
    /* vranlc(0, &t1, A, x) is inlined below */
#pragma spf assert nomacro
    {
      int n4 = 0;
      double a4 = A;
      double *y3 = x;
      //--------------------------------------------------------------------
      //
      //  This routine generates N uniform pseudorandom double precision numbers
      //  in the range (0, 1) by using the linear congruential generator
      //
      //  x_{k+1} = a x_k  (mod 2^46)
      //
      //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
      //  numbers before repeating.  The argument A is the same as 'a' in the
      //  above formula, and X is the same as x_0.  A and X must be odd double
      //  precision integers in the range (1, 2^46).  The N results are placed
      //  in Y and are normalized to be between 0 and 1.  X is updated to
      //  contain the new seed, so that subsequent calls to VRANLC using the
      //  same arguments will generate a continuous sequence.  If N is zero,
      //  only initialization is performed, and the variables X, A and Y are
      //  ignored.
      //
      //  This routine is the standard version designed for scalar or RISC
      //  systems. However, it should produce the same results on any single
      //  processor computer with at least 48 mantissa bits in double precision
      //  floating point data.  On 64 bit systems, double precision should be
      //  disabled.
      //
      //--------------------------------------------------------------------

      // r23 = pow(0.5, 23.0);
      ////  pow(0.5, 23.0) = 1.1920928955078125e-07
      // r46 = r23 * r23;
      // t23 = pow(2.0, 23.0);
      ////  pow(2.0, 23.0) = 8.388608e+06
      // t46 = t23 * t23;

      const double r23 = 1.1920928955078125e-07;
      const double r46 = r23 * r23;
      const double t23 = 8.388608e+06;
      const double t46 = t23 * t23;

      double t1, t2, t3, t4, a1, a2, x1, x2, z;

      int i;

      //--------------------------------------------------------------------
      //  Break A into two parts such that A = 2^23 * A1 + A2.
      //--------------------------------------------------------------------
      t1 = r23 * a4;
      a1 = (int)t1;
      a2 = a4 - t23 * a1;

      //--------------------------------------------------------------------
      //  Generate N results.   This loop is not vectorizable.
      //--------------------------------------------------------------------
      for (i = 0; i < n4; i++) {
        //--------------------------------------------------------------------
        //  Break X into two parts such that X = 2^23 * X1 + X2, compute
        //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
        //  X = 2^23 * Z + A2 * X2  (mod 2^46).
        //--------------------------------------------------------------------
        t1 = r23 * (*x4);
        x1 = (int)t1;
        x2 = *x4 - t23 * x1;
        t1 = a1 * x2 + a2 * x1;
        t2 = (int)(r23 * t1);
        z = t1 - t23 * t2;
        t3 = t23 * z + a2 * x2;
        t4 = (int)(r46 * t3);
        *x4 = t3 - t46 * t4;
        y3[i] = r46 * (*x4);
      }
    }

    //--------------------------------------------------------------------
    //  Compute AN = A ^ (2 * NK) (mod 2^46).
    //--------------------------------------------------------------------

    t1 = A;

    for (i = 0; i < MK + 1; i++) {
      /* randlc(&t1, t1) is inlined below */
      double R1;
#pragma spf assert nomacro
      {
        double *x5 = &t1;
        double a5 = t1;
        //--------------------------------------------------------------------
        //
        //  This routine returns a uniform pseudorandom double precision number
        //  in the range (0, 1) by using the linear congruential generator
        //
        //  x_{k+1} = a x_k  (mod 2^46)
        //
        //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
        //  numbers before repeating.  The argument A is the same as 'a' in the
        //  above formula, and X is the same as x_0.  A and X must be odd double
        //  precision integers in the range (1, 2^46).  The returned value
        //  RANDLC is normalized to be between 0 and 1, i.e. RANDLC = 2^(-46) *
        //  x_1.  X is updated to contain the new seed x_1, so that subsequent
        //  calls to RANDLC using the same arguments will generate a continuous
        //  sequence.
        //
        //  This routine should produce the same results on any computer with at
        //  least 48 mantissa bits in double precision floating point data.  On
        //  64 bit systems, double precision should be disabled.
        //
        //  David H. Bailey     October 26, 1990
        //
        //--------------------------------------------------------------------

        // r23 = pow(0.5, 23.0);
        ////  pow(0.5, 23.0) = 1.1920928955078125e-07
        // r46 = r23 * r23;
        // t23 = pow(2.0, 23.0);
        ////  pow(2.0, 23.0) = 8.388608e+06
        // t46 = t23 * t23;

        const double r23 = 1.1920928955078125e-07;
        const double r46 = r23 * r23;
        const double t23 = 8.388608e+06;
        const double t46 = t23 * t23;

        double t1, t2, t3, t4, a1, a2, x1, x2, z;
        double r;

        //--------------------------------------------------------------------
        //  Break A into two parts such that A = 2^23 * A1 + A2.
        //--------------------------------------------------------------------
        t1 = r23 * a5;
        a1 = (int)t1;
        a2 = a5 - t23 * a1;

        //--------------------------------------------------------------------
        //  Break X into two parts such that X = 2^23 * X1 + X2, compute
        //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
        //  X = 2^23 * Z + A2 * X2  (mod 2^46).
        //--------------------------------------------------------------------
        t1 = r23 * (*x5);
        x1 = (int)t1;
        x2 = *x5 - t23 * x1;
        t1 = a1 * x2 + a2 * x1;
        t2 = (int)(r23 * t1);
        z = t1 - t23 * t2;
        t3 = t23 * z + a2 * x2;
        t4 = (int)(r46 * t3);
        *x5 = t3 - t46 * t4;
        r = r46 * (*x5);

        R1 = r;
      }
      t2 = R1;
    }

    an = t1;
    tt = S;
    gc = 0.0;
    sx = 0.0;
    sy = 0.0;

#pragma omp parallel for default(shared)
    for (i = 0; i < NQ; i++) {
      q[i] = 0.0;
    }

    //--------------------------------------------------------------------
    //  Each instance of this loop may be performed independently. We compute
    //  the k offsets separately to take into account the fact that some nodes
    //  have more numbers to generate than others
    //--------------------------------------------------------------------

    k_offset = -1;

    for (k = 1; k <= np; k++) {
      kk = k_offset + k;
      t1 = S;
      t2 = an;

      // Find starting seed t1 for this kk.

      for (i = 1; i <= 100; i++) {
        ik = kk / 2;
        if ((2 * ik) != kk)
        /* randlc(&t1, t2) is inlined below */
        {
          double R2;
#pragma spf assert nomacro
          {
            double *x6 = &t1;
            double a6 = t2;
            //--------------------------------------------------------------------
            //
            //  This routine returns a uniform pseudorandom double precision
            //  number in the range (0, 1) by using the linear congruential
            //  generator
            //
            //  x_{k+1} = a x_k  (mod 2^46)
            //
            //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates
            //  2^44 numbers before repeating.  The argument A is the same as
            //  'a' in the above formula, and X is the same as x_0.  A and X
            //  must be odd double precision integers in the range (1, 2^46).
            //  The returned value RANDLC is normalized to be between 0 and 1,
            //  i.e. RANDLC = 2^(-46) * x_1.  X is updated to contain the new
            //  seed x_1, so that subsequent calls to RANDLC using the same
            //  arguments will generate a continuous sequence.
            //
            //  This routine should produce the same results on any computer
            //  with at least 48 mantissa bits in double precision floating
            //  point data.  On 64 bit systems, double precision should be
            //  disabled.
            //
            //  David H. Bailey     October 26, 1990
            //
            //--------------------------------------------------------------------

            // r23 = pow(0.5, 23.0);
            ////  pow(0.5, 23.0) = 1.1920928955078125e-07
            // r46 = r23 * r23;
            // t23 = pow(2.0, 23.0);
            ////  pow(2.0, 23.0) = 8.388608e+06
            // t46 = t23 * t23;

            const double r23 = 1.1920928955078125e-07;
            const double r46 = r23 * r23;
            const double t23 = 8.388608e+06;
            const double t46 = t23 * t23;

            double t1, t2, t3, t4, a1, a2, x1, x2, z;
            double r;

            //--------------------------------------------------------------------
            //  Break A into two parts such that A = 2^23 * A1 + A2.
            //--------------------------------------------------------------------
            t1 = r23 * a6;
            a1 = (int)t1;
            a2 = a6 - t23 * a1;

            //--------------------------------------------------------------------
            //  Break X into two parts such that X = 2^23 * X1 + X2, compute
            //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
            //  X = 2^23 * Z + A2 * X2  (mod 2^46).
            //--------------------------------------------------------------------
            t1 = r23 * (*x6);
            x1 = (int)t1;
            x2 = *x6 - t23 * x1;
            t1 = a1 * x2 + a2 * x1;
            t2 = (int)(r23 * t1);
            z = t1 - t23 * t2;
            t3 = t23 * z + a2 * x2;
            t4 = (int)(r46 * t3);
            *x6 = t3 - t46 * t4;
            r = r46 * (*x6);

            R2 = r;
          }
          t3 = R2;
        }
        if (ik == 0)
          break;
        /* randlc(&t2, t2) is inlined below */
        double R3;
#pragma spf assert nomacro
        {
          double *x7 = &t2;
          double a7 = t2;
          //--------------------------------------------------------------------
          //
          //  This routine returns a uniform pseudorandom double precision
          //  number in the range (0, 1) by using the linear congruential
          //  generator
          //
          //  x_{k+1} = a x_k  (mod 2^46)
          //
          //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
          //  numbers before repeating.  The argument A is the same as 'a' in
          //  the above formula, and X is the same as x_0.  A and X must be odd
          //  double precision integers in the range (1, 2^46).  The returned
          //  value RANDLC is normalized to be between 0 and 1, i.e. RANDLC =
          //  2^(-46) * x_1.  X is updated to contain the new seed x_1, so that
          //  subsequent calls to RANDLC using the same arguments will generate
          //  a continuous sequence.
          //
          //  This routine should produce the same results on any computer with
          //  at least 48 mantissa bits in double precision floating point data.
          //  On 64 bit systems, double precision should be disabled.
          //
          //  David H. Bailey     October 26, 1990
          //
          //--------------------------------------------------------------------

          // r23 = pow(0.5, 23.0);
          ////  pow(0.5, 23.0) = 1.1920928955078125e-07
          // r46 = r23 * r23;
          // t23 = pow(2.0, 23.0);
          ////  pow(2.0, 23.0) = 8.388608e+06
          // t46 = t23 * t23;

          const double r23 = 1.1920928955078125e-07;
          const double r46 = r23 * r23;
          const double t23 = 8.388608e+06;
          const double t46 = t23 * t23;

          double t1, t2, t3, t4, a1, a2, x1, x2, z;
          double r;

          //--------------------------------------------------------------------
          //  Break A into two parts such that A = 2^23 * A1 + A2.
          //--------------------------------------------------------------------
          t1 = r23 * a7;
          a1 = (int)t1;
          a2 = a7 - t23 * a1;

          //--------------------------------------------------------------------
          //  Break X into two parts such that X = 2^23 * X1 + X2, compute
          //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
          //  X = 2^23 * Z + A2 * X2  (mod 2^46).
          //--------------------------------------------------------------------
          t1 = r23 * (*x7);
          x1 = (int)t1;
          x2 = *x7 - t23 * x1;
          t1 = a1 * x2 + a2 * x1;
          t2 = (int)(r23 * t1);
          z = t1 - t23 * t2;
          t3 = t23 * z + a2 * x2;
          t4 = (int)(r46 * t3);
          *x7 = t3 - t46 * t4;
          r = r46 * (*x7);

          R3 = r;
        }
        t3 = R3;
        kk = ik;
      }

      //--------------------------------------------------------------------
      //  Compute uniform pseudorandom numbers.
      //--------------------------------------------------------------------
      if (timers_enabled)
        timer_start(2);
        /* vranlc(2 * NK, &t1, A, x) is inlined below */
#pragma spf assert nomacro
      {
        int n5 = 2 * NK;
        double *x8 = &t1;
        double a8 = A;
        double *y4 = x;
        //--------------------------------------------------------------------
        //
        //  This routine generates N uniform pseudorandom double precision
        //  numbers in the range (0, 1) by using the linear congruential
        //  generator
        //
        //  x_{k+1} = a x_k  (mod 2^46)
        //
        //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44
        //  numbers before repeating.  The argument A is the same as 'a' in the
        //  above formula, and X is the same as x_0.  A and X must be odd double
        //  precision integers in the range (1, 2^46).  The N results are placed
        //  in Y and are normalized to be between 0 and 1.  X is updated to
        //  contain the new seed, so that subsequent calls to VRANLC using the
        //  same arguments will generate a continuous sequence.  If N is zero,
        //  only initialization is performed, and the variables X, A and Y are
        //  ignored.
        //
        //  This routine is the standard version designed for scalar or RISC
        //  systems. However, it should produce the same results on any single
        //  processor computer with at least 48 mantissa bits in double
        //  precision floating point data.  On 64 bit systems, double precision
        //  should be disabled.
        //
        //--------------------------------------------------------------------

        // r23 = pow(0.5, 23.0);
        ////  pow(0.5, 23.0) = 1.1920928955078125e-07
        // r46 = r23 * r23;
        // t23 = pow(2.0, 23.0);
        ////  pow(2.0, 23.0) = 8.388608e+06
        // t46 = t23 * t23;

        const double r23 = 1.1920928955078125e-07;
        const double r46 = r23 * r23;
        const double t23 = 8.388608e+06;
        const double t46 = t23 * t23;

        double t1, t2, t3, t4, a1, a2, x1, x2, z;

        int i;

        //--------------------------------------------------------------------
        //  Break A into two parts such that A = 2^23 * A1 + A2.
        //--------------------------------------------------------------------
        t1 = r23 * a8;
        a1 = (int)t1;
        a2 = a8 - t23 * a1;

        //--------------------------------------------------------------------
        //  Generate N results.   This loop is not vectorizable.
        //--------------------------------------------------------------------
        for (i = 0; i < n5; i++) {
          for (i = 0; i < NK; i++) {
            double x_2i, x_2i1;
            {
              //--------------------------------------------------------------------
              //  Break X into two parts such that X = 2^23 * X1 + X2, compute
              //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
              //  X = 2^23 * Z + A2 * X2  (mod 2^46).
              //--------------------------------------------------------------------
              t1 = r23 * (*x4);
              x1 = (int)t1;
              x2 = *x4 - t23 * x1;
              t1 = a1 * x2 + a2 * x1;
              t2 = (int)(r23 * t1);
              z = t1 - t23 * t2;
              t3 = t23 * z + a2 * x2;
              t4 = (int)(r46 * t3);
              *x4 = t3 - t46 * t4;
              x_2i = r46 * (*x4);
            }
            {
              //--------------------------------------------------------------------
              //  Break X into two parts such that X = 2^23 * X1 + X2, compute
              //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
              //  X = 2^23 * Z + A2 * X2  (mod 2^46).
              //--------------------------------------------------------------------
              t1 = r23 * (*x4);
              x1 = (int)t1;
              x2 = *x4 - t23 * x1;
              t1 = a1 * x2 + a2 * x1;
              t2 = (int)(r23 * t1);
              z = t1 - t23 * t2;
              t3 = t23 * z + a2 * x2;
              t4 = (int)(r46 * t3);
              *x4 = t3 - t46 * t4;
              x_2i1 = r46 * (*x4);
            }
            x1 = 2.0 * x_2i - 1.0;
            x2 = 2.0 * x_2i1 - 1.0;
            t1 = x1 * x1 + x2 * x2;
            if (t1 <= 1.0) {
              t2 = sqrt(-2.0 * log(t1) / t1);
              t3 = (x1 * t2);
              t4 = (x2 * t2);
              l = MAX(fabs(t3), fabs(t4));
              q[l] = q[l] + 1.0;
              sx = sx + t3;
              sy = sy + t4;
            }
          }

          if (timers_enabled)
            timer_stop(1);
        }
      }

#pragma omp parallel for default(shared) reduction(+ : gc)
      for (i = 0; i < NQ; i++) {
        gc = gc + q[i];
      }

      timer_stop(0);
      tm = timer_read(0);

      nit = 0;
      verified = true;
      if (M == 24) {
        sx_verify_value = -3.247834652034740e+3;
        sy_verify_value = -6.958407078382297e+3;
      } else if (M == 25) {
        sx_verify_value = -2.863319731645753e+3;
        sy_verify_value = -6.320053679109499e+3;
      } else if (M == 28) {
        sx_verify_value = -4.295875165629892e+3;
        sy_verify_value = -1.580732573678431e+4;
      } else if (M == 30) {
        sx_verify_value = 4.033815542441498e+4;
        sy_verify_value = -2.660669192809235e+4;
      } else if (M == 32) {
        sx_verify_value = 4.764367927995374e+4;
        sy_verify_value = -8.084072988043731e+4;
      } else if (M == 36) {
        sx_verify_value = 1.982481200946593e+5;
        sy_verify_value = -1.020596636361769e+5;
      } else if (M == 40) {
        sx_verify_value = -5.319717441530e+05;
        sy_verify_value = -3.688834557731e+05;
      } else {
        verified = false;
      }

      if (verified) {
        sx_err = fabs((sx - sx_verify_value) / sx_verify_value);
        sy_err = fabs((sy - sy_verify_value) / sy_verify_value);
        verified = ((sx_err <= EPSILON) && (sy_err <= EPSILON));
      }

      Mops = pow(2.0, M + 1) / tm / 1000000.0;

      printf("\nEP Benchmark Results:\n\n");
      printf("CPU Time =%10.4lf\n", tm);
      printf("N = 2^%5d\n", M);
      printf("No. Gaussian Pairs = %15.0lf\n", gc);
      printf("Sums = %25.15lE %25.15lE\n", sx, sy);
      printf("Counts: \n");
      for (i = 0; i < NQ; i++) {
        printf("%3d%15.0lf\n", i, q[i]);
      }

      print_results("EP", CLASS, M + 1, 0, 0, nit, tm, Mops,
                    "Random numbers generated", verified, NPBVERSION,
                    COMPILETIME, CS1, CS2, CS3, CS4, CS5, CS6, CS7);

      if (timers_enabled) {
        if (tm <= 0.0)
          tm = 1.0;
        tt = timer_read(0);
        printf("\nTotal time:     %9.3lf (%6.2lf)\n", tt, tt * 100.0 / tm);
        tt = timer_read(1);
        printf("Gaussian pairs: %9.3lf (%6.2lf)\n", tt, tt * 100.0 / tm);
        tt = timer_read(2);
        printf("Random numbers: %9.3lf (%6.2lf)\n", tt, tt * 100.0 / tm);
      }

      return 0;
    }
  }
}

double randlc(double *x, double a) {
  //--------------------------------------------------------------------
  //
  //  This routine returns a uniform pseudorandom double precision number in the
  //  range (0, 1) by using the linear congruential generator
  //
  //  x_{k+1} = a x_k  (mod 2^46)
  //
  //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44 numbers
  //  before repeating.  The argument A is the same as 'a' in the above formula,
  //  and X is the same as x_0.  A and X must be odd double precision integers
  //  in the range (1, 2^46).  The returned value RANDLC is normalized to be
  //  between 0 and 1, i.e. RANDLC = 2^(-46) * x_1.  X is updated to contain
  //  the new seed x_1, so that subsequent calls to RANDLC using the same
  //  arguments will generate a continuous sequence.
  //
  //  This routine should produce the same results on any computer with at least
  //  48 mantissa bits in double precision floating point data.  On 64 bit
  //  systems, double precision should be disabled.
  //
  //  David H. Bailey     October 26, 1990
  //
  //--------------------------------------------------------------------

  // r23 = pow(0.5, 23.0);
  ////  pow(0.5, 23.0) = 1.1920928955078125e-07
  // r46 = r23 * r23;
  // t23 = pow(2.0, 23.0);
  ////  pow(2.0, 23.0) = 8.388608e+06
  // t46 = t23 * t23;

  const double r23 = 1.1920928955078125e-07;
  const double r46 = r23 * r23;
  const double t23 = 8.388608e+06;
  const double t46 = t23 * t23;

  double t1, t2, t3, t4, a1, a2, x1, x2, z;
  double r;

  //--------------------------------------------------------------------
  //  Break A into two parts such that A = 2^23 * A1 + A2.
  //--------------------------------------------------------------------
  t1 = r23 * a;
  a1 = (int)t1;
  a2 = a - t23 * a1;

  //--------------------------------------------------------------------
  //  Break X into two parts such that X = 2^23 * X1 + X2, compute
  //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
  //  X = 2^23 * Z + A2 * X2  (mod 2^46).
  //--------------------------------------------------------------------
  t1 = r23 * (*x);
  x1 = (int)t1;
  x2 = *x - t23 * x1;
  t1 = a1 * x2 + a2 * x1;
  t2 = (int)(r23 * t1);
  z = t1 - t23 * t2;
  t3 = t23 * z + a2 * x2;
  t4 = (int)(r46 * t3);
  *x = t3 - t46 * t4;
  r = r46 * (*x);

  return r;
}

void vranlc(int n, double *x, double a, double y[]) {
  //--------------------------------------------------------------------
  //
  //  This routine generates N uniform pseudorandom double precision numbers in
  //  the range (0, 1) by using the linear congruential generator
  //
  //  x_{k+1} = a x_k  (mod 2^46)
  //
  //  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44 numbers
  //  before repeating.  The argument A is the same as 'a' in the above formula,
  //  and X is the same as x_0.  A and X must be odd double precision integers
  //  in the range (1, 2^46).  The N results are placed in Y and are normalized
  //  to be between 0 and 1.  X is updated to contain the new seed, so that
  //  subsequent calls to VRANLC using the same arguments will generate a
  //  continuous sequence.  If N is zero, only initialization is performed, and
  //  the variables X, A and Y are ignored.
  //
  //  This routine is the standard version designed for scalar or RISC systems.
  //  However, it should produce the same results on any single processor
  //  computer with at least 48 mantissa bits in double precision floating point
  //  data.  On 64 bit systems, double precision should be disabled.
  //
  //--------------------------------------------------------------------

  // r23 = pow(0.5, 23.0);
  ////  pow(0.5, 23.0) = 1.1920928955078125e-07
  // r46 = r23 * r23;
  // t23 = pow(2.0, 23.0);
  ////  pow(2.0, 23.0) = 8.388608e+06
  // t46 = t23 * t23;

  const double r23 = 1.1920928955078125e-07;
  const double r46 = r23 * r23;
  const double t23 = 8.388608e+06;
  const double t46 = t23 * t23;

  double t1, t2, t3, t4, a1, a2, x1, x2, z;

  int i;

  //--------------------------------------------------------------------
  //  Break A into two parts such that A = 2^23 * A1 + A2.
  //--------------------------------------------------------------------
  t1 = r23 * a;
  a1 = (int)t1;
  a2 = a - t23 * a1;

  //--------------------------------------------------------------------
  //  Generate N results.   This loop is not vectorizable.
  //--------------------------------------------------------------------
  for (i = 0; i < n; i++) {
    //--------------------------------------------------------------------
    //  Break X into two parts such that X = 2^23 * X1 + X2, compute
    //  Z = A1 * X2 + A2 * X1  (mod 2^23), and then
    //  X = 2^23 * Z + A2 * X2  (mod 2^46).
    //--------------------------------------------------------------------
    t1 = r23 * (*x);
    x1 = (int)t1;
    x2 = *x - t23 * x1;
    t1 = a1 * x2 + a2 * x1;
    t2 = (int)(r23 * t1);
    z = t1 - t23 * t2;
    t3 = t23 * z + a2 * x2;
    t4 = (int)(r46 * t3);
    *x = t3 - t46 * t4;
    y[i] = r46 * (*x);
  }

  return;
}

/*  Prototype  */
void wtime(double *);

/*****************************************************************/
/******         E  L  A  P  S  E  D  _  T  I  M  E          ******/
/*****************************************************************/
static double elapsed_time(void) {
  double t;

  wtime(&t);
  return (t);
}

static double start[64], elapsed[64];

/*****************************************************************/
/******            T  I  M  E  R  _  C  L  E  A  R          ******/
/*****************************************************************/
void timer_clear(int n) { elapsed[n] = 0.0; }

/*****************************************************************/
/******            T  I  M  E  R  _  S  T  A  R  T          ******/
/*****************************************************************/
void timer_start(int n) { start[n] = elapsed_time(); }

/*****************************************************************/
/******            T  I  M  E  R  _  S  T  O  P             ******/
/*****************************************************************/
void timer_stop(int n) {
  double t, now;

  now = elapsed_time();
  t = now - start[n];
  elapsed[n] += t;
}

/*****************************************************************/
/******            T  I  M  E  R  _  R  E  A  D             ******/
/*****************************************************************/
double timer_read(int n) { return (elapsed[n]); }

void wtime(double *t) {
  static int sec = -1;
#ifndef _WIN32
  struct timeval tv;
  gettimeofday(&tv, (void *)0);
  if (sec < 0)
    sec = tv.tv_sec;
  *t = (tv.tv_sec - sec) + 1.0e-6 * tv.tv_usec;
#else
  struct _timeb tv;
  _ftime(&tv);
  if (sec < 0)
    sec = tv.time;
  *t = (tv.time - sec) + 1.0e-3 * tv.millitm;
#endif
}

void print_results(char *name, char class, int n1, int n2, int n3, int niter,
                   double t, double mops, char *optype, logical verified,
                   char *npbversion, char *compiletime, char *cs1, char *cs2,
                   char *cs3, char *cs4, char *cs5, char *cs6, char *cs7) {
  char size[16];
  int j;

  printf("\n\n %s Benchmark Completed.\n", name);
  printf(" Class           =             %12c\n", class);

  // If this is not a grid-based problem (EP, FT, CG), then
  // we only print n1, which contains some measure of the
  // problem size. In that case, n2 and n3 are both zero.
  // Otherwise, we print the grid size n1xn2xn3

  if ((n2 == 0) && (n3 == 0)) {
    if ((name[0] == 'E') && (name[1] == 'P')) {
      sprintf(size, "%15.0lf", pow(2.0, n1));
      j = 14;
      if (size[j] == '.') {
        size[j] = ' ';
        j--;
      }
      size[j + 1] = '\0';
      printf(" Size            =          %15s\n", size);
    } else {
      printf(" Size            =             %12d\n", n1);
    }
  } else {
    printf(" Size            =           %4dx%4dx%4d\n", n1, n2, n3);
  }

  printf(" Iterations      =             %12d\n", niter);
  printf(" Time in seconds =             %12.2lf\n", t);
  printf(" Mop/s total     =          %15.2lf\n", mops);
  printf(" Operation type  = %24s\n", optype);
  if (verified)
    printf(" Verification    =             %12s\n", "SUCCESSFUL");
  else
    printf(" Verification    =             %12s\n", "UNSUCCESSFUL");
  printf(" Version         =             %12s\n", npbversion);
  printf(" Compile date    =             %12s\n", compiletime);

  printf("\n Compile options:\n"
         "    CC           = %s\n",
         cs1);
  printf("    CLINK        = %s\n", cs2);
  printf("    C_LIB        = %s\n", cs3);
  printf("    C_INC        = %s\n", cs4);
  printf("    CFLAGS       = %s\n", cs5);
  printf("    CLINKFLAGS   = %s\n", cs6);
  printf("    RAND         = %s\n", cs7);

  printf("\n--------------------------------------\n"
         " Please send all errors/feedbacks to:\n"
         " Center for Manycore Programming\n"
         " cmp@aces.snu.ac.kr\n"
         " http://aces.snu.ac.kr\n"
         "--------------------------------------\n\n");
}
