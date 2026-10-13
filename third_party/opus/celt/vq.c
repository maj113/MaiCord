/* Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2009 Xiph.Org Foundation
   Written by Jean-Marc Valin */
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


#include "mathops.h"
#include "cwrs.h"
#include "vq.h"
#include "arch.h"
#include "os_support.h"
#include "bands.h"
#include "rate.h"
#include "pitch.h"
#include "SigProc_FIX.h"

void norm_scaleup(celt_norm *X, int N, int shift) {
   int i;
   celt_assert(shift >= 0);
   if (shift <= 0) return;
   for (i=0;i<N;i++) X[i] = SHL32(X[i], shift);
}

void norm_scaledown(celt_norm *X, int N, int shift) {
   int i;
   celt_assert(shift >= 0);
   if (shift <= 0) return;
   for (i=0;i<N;i++) X[i] = PSHR32(X[i], shift);
}

opus_val32 celt_inner_prod_norm(const celt_norm *x, const celt_norm *y, int len, int arch) {
   int i;
   opus_val32 sum = 0;
   (void)arch;
   for (i=0;i<len;i++) sum += x[i]*y[i];
   return sum;
}
opus_val32 celt_inner_prod_norm_shift(const celt_norm *x, const celt_norm *y, int len, int arch) {
   int i;
   opus_val64 sum = 0;
   (void)arch;
   for (i=0;i<len;i++) sum += x[i]*(opus_val64)y[i];
   return sum>>2*(NORM_SHIFT-14);
}

#ifndef OVERRIDE_vq_exp_rotation1
static void exp_rotation1(celt_norm *X, int len, int stride, opus_val16 c, opus_val16 s)
{
   int i;
   opus_val16 ms;
   celt_norm *Xptr;
   Xptr = X;
   ms = NEG16(s);
   norm_scaledown(X, len, NORM_SHIFT-14);
   for (i=0;i<len-stride;i++)
   {
      celt_norm x1, x2;
      x1 = Xptr[0];
      x2 = Xptr[stride];
      Xptr[stride] = EXTRACT16(PSHR32(MAC16_16(MULT16_16(c, x2),  s, x1), 15));
      *Xptr++      = EXTRACT16(PSHR32(MAC16_16(MULT16_16(c, x1), ms, x2), 15));
   }
   Xptr = &X[len-2*stride-1];
   for (i=len-2*stride-1;i>=0;i--)
   {
      celt_norm x1, x2;
      x1 = Xptr[0];
      x2 = Xptr[stride];
      Xptr[stride] = EXTRACT16(PSHR32(MAC16_16(MULT16_16(c, x2),  s, x1), 15));
      *Xptr--      = EXTRACT16(PSHR32(MAC16_16(MULT16_16(c, x1), ms, x2), 15));
   }
   norm_scaleup(X, len, NORM_SHIFT-14);
}
#endif /* OVERRIDE_vq_exp_rotation1 */

void exp_rotation(celt_norm *X, int len, int dir, int stride, int K, int spread)
{
   static const int SPREAD_FACTOR[3]={15,10,5};
   int i;
   opus_val16 c, s;
   opus_val16 gain, theta;
   int stride2=0;
   int factor;

   if (2*K>=len || spread==SPREAD_NONE)
      return;
   factor = SPREAD_FACTOR[spread-1];

   gain = celt_div((opus_val32)MULT16_16(Q15_ONE,len),(opus_val32)(len+factor*K));
   theta = HALF16(MULT16_16_Q15(gain,gain));

   c = celt_cos_norm(EXTEND32(theta));
   s = celt_cos_norm(EXTEND32(SUB16(Q15ONE,theta))); /*  sin(theta) */

   if (len>=8*stride)
   {
      stride2 = 1;
      /* This is just a simple (equivalent) way of computing sqrt(len/stride) with rounding.
         It's basically incrementing long as (stride2+0.5)^2 < len/stride. */
      while ((stride2*stride2+stride2)*stride + (stride>>2) < len)
         stride2++;
   }
   /*NOTE: As a minor optimization, we could be passing around log2(B), not B, for both this and for
      extract_collapse_mask().*/
   len = celt_udiv(len, stride);
   for (i=0;i<stride;i++)
   {
      if (dir < 0)
      {
         if (stride2)
            exp_rotation1(X+i*len, len, stride2, s, c);
         exp_rotation1(X+i*len, len, 1, c, s);
      } else {
         exp_rotation1(X+i*len, len, 1, c, -s);
         if (stride2)
            exp_rotation1(X+i*len, len, stride2, s, -c);
      }
   }
}

/** Normalizes the decoded integer pvq codeword to unit norm. */
static void normalise_residual(int * OPUS_RESTRICT iy, celt_norm * OPUS_RESTRICT X,
      int N, opus_val32 Ryy, opus_val32 gain, int shift)
{
   int i;
   int k;
   opus_val32 t;
   opus_val32 g;

   k = celt_ilog2(Ryy)>>1;
   t = VSHR32(Ryy, 2*(k-7)-15);
   g = MULT32_32_Q31(celt_rsqrt_norm32(t),gain);
   i=0;
   (void)shift;
   do X[i] = VSHR32(MULT16_32_Q15(iy[i], g), k+15-NORM_SHIFT);
   while (++i < N);
}

static unsigned extract_collapse_mask(int *iy, int N, int B)
{
   unsigned collapse_mask;
   int N0;
   int i;
   if (B<=1)
      return 1;
   /*NOTE: As a minor optimization, we could be passing around log2(B), not B, for both this and for
      exp_rotation().*/
   N0 = celt_udiv(N, B);
   collapse_mask = 0;
   i=0; do {
      int j;
      unsigned tmp=0;
      j=0; do {
         tmp |= iy[i*N0+j];
      } while (++j<N0);
      collapse_mask |= (tmp!=0)<<i;
   } while (++i<B);
   return collapse_mask;
}

opus_val16 op_pvq_search_c(celt_norm *X, int *iy, int K, int N, int arch)
{
   VARDECL(celt_norm, y);
   VARDECL(int, signx);
   int i, j;
   int pulsesLeft;
   opus_val32 sum;
   opus_val32 xy;
   opus_val16 yy;
   SAVE_STACK;

   (void)arch;
   ALLOC(y, N, celt_norm);
   ALLOC(signx, N, int);
   {
      int shift = (celt_ilog2(1+celt_inner_prod_norm_shift(X, X, N, arch))+1)/2;
      shift = IMAX(0, shift+(NORM_SHIFT-14)-14);
      norm_scaledown(X, N, shift);
   }
   /* Get rid of the sign */
   sum = 0;
   j=0; do {
      signx[j] = X[j]<0;
      /* OPT: Make sure the compiler doesn't use a branch on ABS16(). */
      X[j] = ABS16(X[j]);
      iy[j] = 0;
      y[j] = 0;
   } while (++j<N);

   xy = yy = 0;

   pulsesLeft = K;

   /* Do a pre-search by projecting on the pyramid */
   if (K > (N>>1))
   {
      opus_val16 rcp;
      j=0; do {
         sum += X[j];
      }  while (++j<N);

      /* If X is too small, just replace it with a pulse at 0 */
      if (sum <= K)
      {
         X[0] = QCONST16(1.f,14);
         j=1; do
            X[j]=0;
         while (++j<N);
         sum = QCONST16(1.f,14);
      }
      rcp = EXTRACT16(MULT16_32_Q16(K, celt_rcp(sum)));
      j=0; do {
         /* It's really important to round *towards zero* here */
         iy[j] = MULT16_16_Q15(X[j],rcp);
         y[j] = (celt_norm)iy[j];
         yy = MAC16_16(yy, y[j],y[j]);
         xy = MAC16_16(xy, X[j],y[j]);
         y[j] *= 2;
         pulsesLeft -= iy[j];
      }  while (++j<N);
   }
   celt_sig_assert(pulsesLeft>=0);

   /* This should never happen, but just in case it does (e.g. on silence)
      we fill the first bin with pulses. */
   if (pulsesLeft > N+3)
   {
      opus_val16 tmp = (opus_val16)pulsesLeft;
      yy = MAC16_16(yy, tmp, tmp);
      yy = MAC16_16(yy, tmp, y[0]);
      iy[0] += pulsesLeft;
      pulsesLeft=0;
   }

   for (i=0;i<pulsesLeft;i++)
   {
      opus_val16 Rxy, Ryy;
      int best_id;
      opus_val32 best_num;
      opus_val16 best_den;
      int rshift;
      rshift = 1+celt_ilog2(K-pulsesLeft+i+1);
      best_id = 0;
      /* The squared magnitude term gets added anyway, so we might as well
         add it outside the loop */
      yy = ADD16(yy, 1);

      /* Calculations for position 0 are out of the loop, in part to reduce
         mispredicted branches (since the if condition is usually false)
         in the loop. */
      /* Temporary sums of the new pulse(s) */
      Rxy = EXTRACT16(SHR32(ADD32(xy, EXTEND32(X[0])),rshift));
      /* We're multiplying y[j] by two so we don't have to do it here */
      Ryy = ADD16(yy, y[0]);

      /* Approximate score: we maximise Rxy/sqrt(Ryy) (we're guaranteed that
         Rxy is positive because the sign is pre-computed) */
      Rxy = MULT16_16_Q15(Rxy,Rxy);
      best_den = Ryy;
      best_num = Rxy;
      j=1;
      do {
         /* Temporary sums of the new pulse(s) */
         Rxy = EXTRACT16(SHR32(ADD32(xy, EXTEND32(X[j])),rshift));
         /* We're multiplying y[j] by two so we don't have to do it here */
         Ryy = ADD16(yy, y[j]);

         /* Approximate score: we maximise Rxy/sqrt(Ryy) (we're guaranteed that
            Rxy is positive because the sign is pre-computed) */
         Rxy = MULT16_16_Q15(Rxy,Rxy);
         /* The idea is to check for num/den >= best_num/best_den, but that way
            we can do it without any division */
         /* OPT: It's not clear whether a cmov is faster than a branch here
            since the condition is more often false than true and using
            a cmov introduces data dependencies across iterations. The optimal
            choice may be architecture-dependent. */
         if (opus_unlikely(MULT16_16(best_den, Rxy) > MULT16_16(Ryy, best_num)))
         {
            best_den = Ryy;
            best_num = Rxy;
            best_id = j;
         }
      } while (++j<N);

      /* Updating the sums of the new pulse(s) */
      xy = ADD32(xy, EXTEND32(X[best_id]));
      /* We're multiplying y[j] by two so we don't have to do it here */
      yy = ADD16(yy, y[best_id]);

      /* Only now that we've made the final choice, update y/iy */
      /* Multiplying y[j] by 2 so we don't have to do it everywhere else */
      y[best_id] += 2;
      iy[best_id]++;
   }

   /* Put the original sign back */
   j=0;
   do {
      /*iy[j] = signx[j] ? -iy[j] : iy[j];*/
      /* OPT: The is more likely to be compiled without a branch than the code above
         but has the same performance otherwise. */
      iy[j] = (iy[j]^-signx[j]) + signx[j];
   } while (++j<N);
   RESTORE_STACK;
   return yy;
}


unsigned alg_quant(celt_norm *X, int N, int K, int spread, int B, ec_enc *enc,
      opus_val32 gain, int resynth
, int arch)
{
   VARDECL(int, iy);
   opus_val32 yy;
   unsigned collapse_mask;
   SAVE_STACK;

   celt_assert2(K>0, "alg_quant() needs at least one pulse");
   celt_assert2(N>1, "alg_quant() needs at least two dimensions");

   /* Covers vectorization by up to 4. */
   ALLOC(iy, N+3, int);

   exp_rotation(X, N, 1, B, K, spread);

   {
      yy = op_pvq_search(X, iy, K, N, arch);
      collapse_mask = extract_collapse_mask(iy, N, B);
      encode_pulses(iy, N, K, enc);
      if (resynth)
         normalise_residual(iy, X, N, yy, gain, 0);
   }

   if (resynth)
      exp_rotation(X, N, -1, B, K, spread);

   RESTORE_STACK;
   return collapse_mask;
}

/** Decode pulse vector and combine the result with the pitch vector to produce
    the final normalised signal in the current band. */
unsigned alg_unquant(celt_norm *X, int N, int K, int spread, int B,
      ec_dec *dec, opus_val32 gain)
{
   opus_val32 Ryy;
   unsigned collapse_mask;
   VARDECL(int, iy);
   int yy_shift=0;
   SAVE_STACK;

   celt_assert2(K>0, "alg_unquant() needs at least one pulse");
   celt_assert2(N>1, "alg_unquant() needs at least two dimensions");
   ALLOC(iy, N, int);
   Ryy = decode_pulses(iy, N, K, dec);
   normalise_residual(iy, X, N, Ryy, gain, yy_shift);
   exp_rotation(X, N, -1, B, K, spread);
   collapse_mask = extract_collapse_mask(iy, N, B);
   RESTORE_STACK;
   return collapse_mask;
}

#ifndef OVERRIDE_renormalise_vector
void renormalise_vector(celt_norm *X, int N, opus_val32 gain, int arch)
{
   int i;
   int k;
   opus_val32 E;
   opus_val16 g;
   opus_val32 t;
   celt_norm *xptr;
   norm_scaledown(X, N, NORM_SHIFT-14);
   E = EPSILON + celt_inner_prod_norm(X, X, N, arch);
   k = celt_ilog2(E)>>1;
   t = VSHR32(E, 2*(k-7));
   g = MULT32_32_Q31(celt_rsqrt_norm(t),gain);

   xptr = X;
   for (i=0;i<N;i++)
   {
      *xptr = EXTRACT16(PSHR32(MULT16_16(g, *xptr), k+15-14));
      xptr++;
   }
   norm_scaleup(X, N, NORM_SHIFT-14);
   /*return celt_sqrt(E);*/
}
#endif /* OVERRIDE_renormalise_vector */

opus_int32 stereo_itheta(const celt_norm *X, const celt_norm *Y, int stereo, int N, int arch)
{
   int i;
   int itheta;
   opus_val32 mid, side;
   opus_val32 Emid, Eside;

   Emid = Eside = 0;
   if (stereo)
   {
      for (i=0;i<N;i++)
      {
         celt_norm m, s;
         m = PSHR32(ADD32(X[i], Y[i]), NORM_SHIFT-13);
         s = PSHR32(SUB32(X[i], Y[i]), NORM_SHIFT-13);
         Emid = MAC16_16(Emid, m, m);
         Eside = MAC16_16(Eside, s, s);
      }
   } else {
      Emid += celt_inner_prod_norm_shift(X, X, N, arch);
      Eside += celt_inner_prod_norm_shift(Y, Y, N, arch);
   }
   mid = celt_sqrt32(Emid);
   side = celt_sqrt32(Eside);
   itheta = celt_atan2p_norm(side, mid);

   return itheta;
}

