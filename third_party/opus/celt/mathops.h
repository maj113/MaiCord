/* Copyright (c) 2002-2008 Jean-Marc Valin
   Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2009 Xiph.Org Foundation
   Copyright (c) 2024 Arm Limited
   Written by Jean-Marc Valin, and Yunho Huh */
/**
   @file mathops.h
   @brief Various math functions
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MATHOPS_H
#define MATHOPS_H

#include "arch.h"
#include "entcode.h"
#include "os_support.h"


#define PI 3.1415926535897931

/* Multiplies two 16-bit fractional values. Bit-exactness of this macro is important */
#define FRAC_MUL16(a,b) ((16384+((opus_int32)(opus_int16)(a)*(opus_int16)(b)))>>15)

unsigned isqrt32(opus_uint32 _val);

/* CELT doesn't need it for fixed-point, by analysis.c does. */


#ifndef OVERRIDE_CELT_MAXABS16
static OPUS_INLINE opus_val32 celt_maxabs16(const opus_val16 *x, int len)
{
   int i;
   opus_val16 maxval = 0;
   opus_val16 minval = 0;
   for (i=0;i<len;i++)
   {
      maxval = MAX16(maxval, x[i]);
      minval = MIN16(minval, x[i]);
   }
   return MAX32(EXTEND32(maxval),-EXTEND32(minval));
}
#endif

#define celt_maxabs_res celt_maxabs16


#ifndef OVERRIDE_CELT_MAXABS32
static OPUS_INLINE opus_val32 celt_maxabs32(const opus_val32 *x, int len)
{
   int i;
   opus_val32 maxval = 0;
   opus_val32 minval = 0;
   for (i=0;i<len;i++)
   {
      maxval = MAX32(maxval, x[i]);
      minval = MIN32(minval, x[i]);
   }
   return MAX32(maxval, -minval);
}
#endif


#include "os_support.h"

#ifndef OVERRIDE_CELT_ILOG2
/** Integer log in base2. Undefined for zero and negative numbers */
static OPUS_INLINE opus_int16 celt_ilog2(opus_int32 x)
{
   celt_sig_assert(x>0);
   return EC_ILOG(x)-1;
}
#endif


/** Integer log in base2. Defined for zero, but not for negative numbers */
static OPUS_INLINE opus_int16 celt_zlog2(opus_val32 x)
{
   return x <= 0 ? 0 : celt_ilog2(x);
}

opus_val16 celt_rsqrt_norm(opus_val32 x);

opus_val32 celt_rsqrt_norm32(opus_val32 x);

opus_val32 celt_sqrt(opus_val32 x);

opus_val32 celt_sqrt32(opus_val32 x);

opus_val16 celt_cos_norm(opus_val32 x);


/** Base-2 logarithm approximation (log2(x)). (Q14 input, Q10 output) */
static OPUS_INLINE opus_val16 celt_log2(opus_val32 x)
{
   int i;
   opus_val16 n, frac;
   /* -0.41509302963303146, 0.9609890551383969, -0.31836011537636605,
       0.15530808010959576, -0.08556153059057618 */
   static const opus_val16 C[5] = {-6801+(1<<(13-10)), 15746, -5217, 2545, -1401};
   if (x==0)
      return -32767;
   i = celt_ilog2(x);
   n = VSHR32(x,i-15)-32768-16384;
   frac = ADD16(C[0], MULT16_16_Q15(n, ADD16(C[1], MULT16_16_Q15(n, ADD16(C[2], MULT16_16_Q15(n, ADD16(C[3], MULT16_16_Q15(n, C[4]))))))));
   return SHL32(i-13,10)+SHR32(frac,14-10);
}

/*
 K0 = 1
 K1 = log(2)
 K2 = 3-4*log(2)
 K3 = 3*log(2) - 2
*/
#define D0 16383
#define D1 22804
#define D2 14819
#define D3 10204

static OPUS_INLINE opus_val32 celt_exp2_frac(opus_val16 x)
{
   opus_val16 frac;
   frac = SHL16(x, 4);
   return ADD16(D0, MULT16_16_Q15(frac, ADD16(D1, MULT16_16_Q15(frac, ADD16(D2 , MULT16_16_Q15(D3,frac))))));
}

#undef D0
#undef D1
#undef D2
#undef D3

/** Base-2 exponential approximation (2^x). (Q10 input, Q16 output) */
static OPUS_INLINE opus_val32 celt_exp2(opus_val16 x)
{
   int integer;
   opus_val16 frac;
   integer = SHR16(x,10);
   if (integer>14)
      return 0x7f000000;
   else if (integer < -15)
      return 0;
   frac = celt_exp2_frac(x-SHL16(integer,10));
   return VSHR32(EXTEND32(frac), -integer-2);
}


#define celt_log2_db(x) SHL32(EXTEND32(celt_log2(x)), DB_SHIFT-10)
#define celt_exp2_db_frac(x) SHL32(celt_exp2_frac(PSHR32(x, DB_SHIFT-10)), 14)
#define celt_exp2_db(x) celt_exp2(PSHR32(x, DB_SHIFT-10))


opus_val32 celt_rcp(opus_val32 x);
opus_val32 celt_rcp_norm32(opus_val32 x);

#define celt_div(a,b) MULT32_32_Q31((opus_val32)(a),celt_rcp(b))

opus_val32 frac_div32_q29(opus_val32 a, opus_val32 b);
opus_val32 frac_div32(opus_val32 a, opus_val32 b);

/* Computes atan(x) multiplied by 2/PI. The input value (x) should be within the
 * range of -1 to 1 and represented in Q30 format. The function will return the
 * result in Q30 format. */
static OPUS_INLINE opus_val32 celt_atan_norm(opus_val32 x)
{
   /* Approximation constants. */
   static const opus_int32 ATAN_2_OVER_PI = 1367130551;   /* Q31 */
   static const opus_int32 ATAN_COEFF_A03 = -715791936;   /* Q31 */
   static const opus_int32 ATAN_COEFF_A05 = 857391616;    /* Q32 */
   static const opus_int32 ATAN_COEFF_A07 = -1200579328;  /* Q33 */
   static const opus_int32 ATAN_COEFF_A09 = 1682636672;   /* Q34 */
   static const opus_int32 ATAN_COEFF_A11 = -1985085440;  /* Q35 */
   static const opus_int32 ATAN_COEFF_A13 = 1583306112;   /* Q36 */
   static const opus_int32 ATAN_COEFF_A15 = -598602432;   /* Q37 */
   opus_int32 x_sq_q30;
   opus_int32 x_q31;
   opus_int32 tmp;
   /* The expected x is in the range of [-1.0f, 1.0f] */
   celt_sig_assert((x <= 1073741824) && (x >= -1073741824));

   /* If x = 1.0f, returns 0.5f */
   if (x == 1073741824)
   {
      return 536870912; /* 0.5f (Q30) */
   }
   /* If x = 1.0f, returns 0.5f */
   if (x == -1073741824)
   {
      return -536870912; /* -0.5f (Q30) */
   }
   x_q31 = SHL32(x, 1);
   x_sq_q30 = MULT32_32_Q31(x_q31, x);
   /* Split evaluation in steps to avoid exploding macro expansion. */
   tmp = MULT32_32_Q31(x_sq_q30, ATAN_COEFF_A15);
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A13, tmp));
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A11, tmp));
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A09, tmp));
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A07, tmp));
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A05, tmp));
   tmp = MULT32_32_Q31(x_sq_q30, ADD32(ATAN_COEFF_A03, tmp));
   tmp = ADD32(x, MULT32_32_Q31(x_q31, tmp));
   return MULT32_32_Q31(ATAN_2_OVER_PI, tmp);
}

/* Calculates the arctangent of y/x, multiplies the result by 2/pi, and returns
 * the value in Q30 format. Both input values (x and y) must be within the range
 * of 0 to 1 and represented in Q30 format. Inputs must be zero or greater, and
 * at least one input must be non-zero. */
static OPUS_INLINE opus_val32 celt_atan2p_norm(opus_val32 y, opus_val32 x)
{
   celt_sig_assert(x>=0 && y>=0);
   if (y==0 && x==0) {
      return 0;
   } else if (y < x) {
      return celt_atan_norm(SHR32(frac_div32(y, x), 1));
   } else {
      celt_sig_assert(y > 0);
      return 1073741824 /* 1.0f Q30 */ -
             celt_atan_norm(SHR32(frac_div32(x, y), 1));
   }
}

#define M1 32767
#define M2 -21
#define M3 -11943
#define M4 4936

/* Atan approximation using a 4th order polynomial. Input is in Q15 format
   and normalized by pi/4. Output is in Q15 format */
static OPUS_INLINE opus_val16 celt_atan01(opus_val16 x)
{
   return MULT16_16_P15(x, ADD32(M1, MULT16_16_P15(x, ADD32(M2, MULT16_16_P15(x, ADD32(M3, MULT16_16_P15(M4, x)))))));
}

#undef M1
#undef M2
#undef M3
#undef M4

/* atan2() approximation valid for positive input values */
static OPUS_INLINE opus_val16 celt_atan2p(opus_val16 y, opus_val16 x)
{
   if (x==0 && y==0) {
      return 0;
   } else if (y < x)
   {
      opus_val32 arg;
      arg = celt_div(SHL32(EXTEND32(y),15),x);
      if (arg >= 32767)
         arg = 32767;
      return SHR16(celt_atan01(EXTRACT16(arg)),1);
   } else {
      opus_val32 arg;
      arg = celt_div(SHL32(EXTEND32(x),15),y);
      if (arg >= 32767)
         arg = 32767;
      return 25736-SHR16(celt_atan01(EXTRACT16(arg)),1);
   }
}


#endif /* MATHOPS_H */
