/*--------------------------------------------------------------------

  NAS Parallel Benchmarks 3.0 structured OpenMP C versions - EP
  This benchmark is an OpenMP C version of the NPB EP code.

  The OpenMP C 2.3 versions are derived by RWCP from the serial Fortran versions
  in "NPB 2.3-serial" developed by NAS. 3.0 translation is performed by the UVSQ.
  Permission to use, copy, distribute and modify this software for any
  purpose with or without fee is hereby granted.
  This software is provided "as is" without express or implied warranty.

  Information on OpenMP activities at RWCP is available at:
           http://pdplab.trc.rwcp.or.jp/pdperf/Omni/

  Information on NAS Parallel Benchmarks 2.3 is available at:

           http://www.nas.nasa.gov/NAS/NPB/
--------------------------------------------------------------------*/
/*--------------------------------------------------------------------
  Author: P. O. Frederickson
          D. H. Bailey
          A. C. Woo
  OpenMP C version: S. Satoh

  3.0 structure translation: M. Popov

--------------------------------------------------------------------*/

#include "npbparams.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

typedef int boolean;
typedef struct {
  double real;
  double imag;
} dcomplex;

#define TRUE  1
#define FALSE  0

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define  pow2(a) ((a)*(a))

#define get_real(c) c.real
#define get_imag(c) c.imag
#define cadd(c, a, b) (c.real = a.real + b.real, c.imag = a.imag + b.imag)
#define csub(c, a, b) (c.real = a.real - b.real, c.imag = a.imag - b.imag)
#define cmul(c, a, b) (c.real = a.real * b.real - a.imag * b.imag, \
                     c.imag = a.real * b.imag + a.imag * b.real)
#define crmul(c, a, b) (c.real = a.real * b, c.imag = a.imag * b)

/* parameters */
#define  MK    16
#define  MM    (M - MK)
#define  NN    (1 << MM)
#define  NK    (1 << MK)
#define  NQ    10
#define EPSILON    1.0e-8
#define  A    1220703125.0
#define  S    271828183.0
#define  TIMERS_ENABLED  FALSE

/*
*/
#if defined(USE_POW)
#define r23 pow(0.5, 23.0)
#define r46 (r23*r23)
#define t23 pow(2.0, 23.0)
#define t46 (t23*t23)
#else
#define r23 (0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5*0.5)
#define r46 (r23*r23)
#define t23 (2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0*2.0)
#define t46 (t23*t23)
#endif

/*c---------------------------------------------------------------------
c---------------------------------------------------------------------*/

double randlc(double *x, double a) {

/*c---------------------------------------------------------------------
c---------------------------------------------------------------------*/

/*c---------------------------------------------------------------------
c
c   This routine returns a uniform pseudorandom double precision number in the
c   range (0, 1) by using the linear congruential generator
c
c   x_{k+1} = a x_k  (mod 2^46)
c
c   where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44 numbers
c   before repeating.  The argument A is the same as 'a' in the above formula,
c   and X is the same as x_0.  A and X must be odd double precision integers
c   in the range (1, 2^46).  The returned value RANDLC is normalized to be
c   between 0 and 1, i.e. RANDLC = 2^(-46) * x_1.  X is updated to contain
c   the new seed x_1, so that subsequent calls to RANDLC using the same
c   arguments will generate a continuous sequence.
c
c   This routine should produce the same results on any computer with at least
c   48 mantissa bits in double precision floating point data.  On 64 bit
c   systems, double precision should be disabled.
c
c   David H. Bailey     October 26, 1990
c
c---------------------------------------------------------------------*/

  double t1, t2, t3, t4, a1, a2, x1, x2, z;

/*c---------------------------------------------------------------------
c   Break A into two parts such that A = 2^23 * A1 + A2.
c---------------------------------------------------------------------*/
  t1 = r23 * a;
  a1 = (int) t1;
  a2 = a - t23 * a1;

/*c---------------------------------------------------------------------
c   Break X into two parts such that X = 2^23 * X1 + X2, compute
c   Z = A1 * X2 + A2 * X1  (mod 2^23), and then
c   X = 2^23 * Z + A2 * X2  (mod 2^46).
c---------------------------------------------------------------------*/
  t1 = r23 * (*x);
  x1 = (int) t1;
  x2 = (*x) - t23 * x1;
  t1 = a1 * x2 + a2 * x1;
  t2 = (int) (r23 * t1);
  z = t1 - t23 * t2;
  t3 = t23 * z + a2 * x2;
  t4 = (int) (r46 * t3);
  (*x) = t3 - t46 * t4;

  return (r46 * (*x));
}

/*c---------------------------------------------------------------------
c---------------------------------------------------------------------*/

void vranlc(int n, double *x_seed, double a, double y[]) {

/*c---------------------------------------------------------------------
c---------------------------------------------------------------------*/

/*c---------------------------------------------------------------------
c
c   This routine generates N uniform pseudorandom double precision numbers in
c   the range (0, 1) by using the linear congruential generator
c
c   x_{k+1} = a x_k  (mod 2^46)
c
c   where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44 numbers
c   before repeating.  The argument A is the same as 'a' in the above formula,
c   and X is the same as x_0.  A and X must be odd double precision integers
c   in the range (1, 2^46).  The N results are placed in Y and are normalized
c   to be between 0 and 1.  X is updated to contain the new seed, so that
c   subsequent calls to VRANLC using the same arguments will generate a
c   continuous sequence.  If N is zero, only initialization is performed, and
c   the variables X, A and Y are ignored.
c
c   This routine is the standard version designed for scalar or RISC systems.
c   However, it should produce the same results on any single processor
c   computer with at least 48 mantissa bits in double precision floating point
c   data.  On 64 bit systems, double precision should be disabled.
c
c---------------------------------------------------------------------*/

  int i;
  double x, t1, t2, t3, t4, a1, a2, x1, x2, z;

/*c---------------------------------------------------------------------
c   Break A into two parts such that A = 2^23 * A1 + A2.
c---------------------------------------------------------------------*/
  t1 = r23 * a;
  a1 = (int) t1;
  a2 = a - t23 * a1;
  x = *x_seed;

/*c---------------------------------------------------------------------
c   Generate N results.   This loop is not vectorizable.
c---------------------------------------------------------------------*/
  for (i = 1; i <= n; i++) {

/*c---------------------------------------------------------------------
c   Break X into two parts such that X = 2^23 * X1 + X2, compute
c   Z = A1 * X2 + A2 * X1  (mod 2^23), and then
c   X = 2^23 * Z + A2 * X2  (mod 2^46).
c---------------------------------------------------------------------*/
    t1 = r23 * x;
    x1 = (int) t1;
    x2 = x - t23 * x1;
    t1 = a1 * x2 + a2 * x1;
    t2 = (int) (r23 * t1);
    z = t1 - t23 * t2;
    t3 = t23 * z + a2 * x2;
    t4 = (int) (r46 * t3);
    x = t3 - t46 * t4;
    y[i] = r46 * x;
  }
  *x_seed = x;
}

/* global variables */
/* common /storage/ */
static double x[2 * NK];
static double q[NQ];

/*--------------------------------------------------------------------
      program EMBAR
c-------------------------------------------------------------------*/
/*
c   This is the serial version of the APP Benchmark 1,
c   the "embarassingly parallel" benchmark.
c
c   M is the Log_2 of the number of complex pairs of uniform (0, 1) random
c   numbers.  MK is the Log_2 of the size of each batch of uniform random
c   numbers.  MK can be set for convenience on a given system, since it does
c   not affect the results.
*/
int main(int argc, char **argv) {
  double Mops, t1, t2, t3, t4, x1, x2, sx, sy, tm, an, tt, gc;
  double dum[3] = {1.0, 1.0, 1.0};
  int np, ierr, node, no_nodes, i, ik, kk, l, k, nit, ierrcode,
          no_large_nodes, np_add, k_offset, j;
  int nthreads = 1;
  boolean verified;
  char size[13 + 1];  /* character*13 */

  {
    double t1, t2, t3, t4, x1, x2;
    int kk, i, ik, l;
    double qq[NQ];    /* private copy of q[0:NQ-1] */

    for (i = 0; i < NQ; i++) qq[i] = 0.0;

    // PARALLEL
    for (k = 1; k <= np; k++) {
      kk = k_offset + k;
      t1 = S;
      t2 = an;

      for (i = 1; i <= 100; i++) {
        ik = kk / 2;
        if (2 * ik != kk) t3 = randlc(&t1, t2);         // <------ CALL
        if (ik == 0) break;
        t3 = randlc(&t2, t2);                           // <------ CALL
        kk = ik;
      }

      vranlc(2 * NK, &t1, A, x - 1);             // <------ CALL

      for (i = 0; i < NK; i++) {
        x1 = 2.0 * x[2 * i] - 1.0;
        x2 = 2.0 * x[2 * i + 1] - 1.0;
        t1 = pow2(x1) + pow2(x2);                      // <------ CALL
        if (t1 <= 1.0) {
          t2 = sqrt(-2.0 * log(t1) / t1);              // <------ CALL
          t3 = (x1 * t2);        /* Xi */
          t4 = (x2 * t2);        /* Yi */
          l = max(fabs(t3), fabs(t4));          // <------ CALL
          qq[l] += 1.0;        /* counts */
          sx = sx + t3;        /* sum of Xi */
          sy = sy + t4;        /* sum of Yi */
        }
      }
    }
  } /* end of parallel region */
}




int *glob;

void fun1(int *a, int *b) {  // Function [fun1]: 	a,
    b[11] = b[12];
    glob = a;
}

int* fun2(int *a, int *b) {  // Function [fun2]: 	b,
    return b;
}

int* fun3(int *a, int *b) {  // Function [fun3]: 	b,
    int *c = b;
    return c;
}

void fun4(int *a, int *b) {  // Function [fun4]: 	a,
    int *d = &(b[10]);
    int *c = &(a[10]);
    glob = c;
}

void fun5(int *a, int **b) {  // Function [fun5]: 	a,
    int *c = &(a[10]);
    *b = c;
}

int **addr = (int **) 11231231;
int **getAddr() {
    return addr;
}

void fun6(int *a, int *b) {  // Function [fun6]: 	b,
    int **addr = getAddr();
    *addr = b;
}

void fun7(int *a, int *b) {  // Function [fun7]: 	a,
    fun6(b, a);
}

void fun8(int *a, int *b) {  // Function [fun8]: 	a,
    b[1] = b[2];
    int **c = &a;   // c -> glob, a -> 1
    glob = *c;
}
