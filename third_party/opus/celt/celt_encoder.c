/* Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2010 Xiph.Org Foundation
   Copyright (c) 2008 Gregory Maxwell
   Written by Jean-Marc Valin and Gregory Maxwell */
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


#define CELT_ENCODER_C

#include "os_support.h"
#include "mdct.h"
#include <math.h>
#include "celt.h"
#include "pitch.h"
#include "bands.h"
#include "modes.h"
#include "entcode.h"
#include "quant_bands.h"
#include "rate.h"
#include "stack_alloc.h"
#include "mathops.h"
#include "float_cast.h"
#include <stdarg.h>
#include "celt_lpc.h"
#include "vq.h"


#ifndef M_PI
#define M_PI 3.141592653
#endif


/* The one frame shape the encoder codes: a 20 ms hybrid frame (LM 3, 960 samples) whose CELT layer carries
   the bands from CELT_HYBRID_START up; the SILK layer codes the rest. */
#define CELT_FRAME_LM 3
#define CELT_FRAME_SIZE 960
#define CELT_HYBRID_START 17

/** Encoder state
 @brief Encoder state
 */
struct OpusCustomEncoder {
   const OpusCustomMode *mode;     /**< Mode used by the encoder */
   int channels;
   int stream_channels;

   int complexity;
   opus_int32 bitrate;
   int loss_rate;

   opus_uint32 rng;
   opus_val32 delayedIntra;
   int lastCodedBands;
   int consec_transient;
   SILKInfo silk_info;

   opus_val32 preemph_memE[2];
   int intensity;

   celt_sig in_mem[1]; /* Size = channels*mode->overlap */
   /* celt_glog oldBandE[],     Size = channels*mode->nbEBands */
   /* celt_glog oldLogE[],      Size = channels*mode->nbEBands */
   /* celt_glog oldLogE2[],     Size = channels*mode->nbEBands */
   /* celt_glog energyError[],  Size = channels*mode->nbEBands */
};

int celt_encoder_get_size(int channels)
{
   CELTMode *mode = opus_custom_mode_create(48000, 960, NULL);
   return opus_custom_encoder_get_size(mode, channels);
}

OPUS_CUSTOM_NOSTATIC int opus_custom_encoder_get_size(const CELTMode *mode, int channels)
{
   int extra=0;
   int size;
   size = sizeof(struct CELTEncoder)
         + (channels*mode->overlap-1)*sizeof(celt_sig)    /* celt_sig in_mem[channels*mode->overlap]; */
         + 4*channels*mode->nbEBands*sizeof(celt_glog)    /* celt_glog oldBandE[channels*mode->nbEBands]; */
                                                          /* celt_glog oldLogE[channels*mode->nbEBands]; */
                                                          /* celt_glog oldLogE2[channels*mode->nbEBands]; */
                                                          /* celt_glog energyError[channels*mode->nbEBands]; */
         + extra;
   return size;
}


int celt_encoder_init(CELTEncoder *st, int channels)
{
   const CELTMode *mode = opus_custom_mode_create(48000, 960, NULL);
   celt_glog *oldLogE;
   int i;
   if (channels < 0 || channels > 2)
      return OPUS_BAD_ARG;

   if (st==NULL || mode==NULL)
      return OPUS_ALLOC_FAIL;

   OPUS_CLEAR((char*)st, opus_custom_encoder_get_size(mode, channels));

   st->mode = mode;
   st->stream_channels = st->channels = channels;

   st->complexity = 5;
   st->delayedIntra = 1;

   /* oldLogE and oldLogE2 follow oldBandE */
   oldLogE = (celt_glog*)(st->in_mem+channels*mode->overlap) + channels*mode->nbEBands;
   for (i=0;i<2*channels*mode->nbEBands;i++)
      oldLogE[i] = -GCONST(28.f);

   return OPUS_OK;
}


static int transient_analysis(const opus_val32 * OPUS_RESTRICT in, int len, int C,
                              opus_val16 *tf_estimate, int allow_weak_transients,
                              int *weak_transient, opus_val16 tone_freq, opus_val32 toneishness, char *scratch)
{
   int i;
   VARDECL(opus_val16, tmp);
   opus_val32 mem0,mem1;
   int is_transient = 0;
   opus_int32 mask_metric = 0;
   int c;
   opus_val16 tf_max;
   int len2;
   /* Forward masking: 6.7 dB/ms. */
   int forward_shift = 4;
   /* Table of 6*64/x, trained on real data to minimize the average error */
   static const unsigned char inv_table[128] = {
         255,255,156,110, 86, 70, 59, 51, 45, 40, 37, 33, 31, 28, 26, 25,
          23, 22, 21, 20, 19, 18, 17, 16, 16, 15, 15, 14, 13, 13, 12, 12,
          12, 12, 11, 11, 11, 10, 10, 10,  9,  9,  9,  9,  9,  9,  8,  8,
           8,  8,  8,  7,  7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,
           6,  6,  6,  6,  6,  6,  6,  6,  6,  5,  5,  5,  5,  5,  5,  5,
           5,  5,  5,  5,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
           4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,
           3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,
   };
   SAVE_STACK;
   int in_shift = IMAX(0, celt_ilog2(1+celt_maxabs32(in, C*len))-14);
   ALLOC(tmp, len, opus_val16);

   *weak_transient = 0;
   /* For lower bitrates, let's be more conservative and have a forward masking
      decay of 3.3 dB/ms. This avoids having to code transients at very low
      bitrate (mostly for hybrid), which can result in unstable energy and/or
      partial collapse. */
   if (allow_weak_transients)
   {
      forward_shift = 5;
   }
   len2=len/2;
   for (c=0;c<C;c++)
   {
      opus_val32 mean;
      opus_int32 unmask=0;
      opus_val32 norm;
      opus_val16 maxE;
      mem0=0;
      mem1=0;
      /* High-pass filter: (1 - 2*z^-1 + z^-2) / (1 - z^-1 + .5*z^-2) */
      for (i=0;i<len;i++)
      {
         opus_val32 x,y;
         x = SHR32(in[i+c*len],in_shift);
         y = ADD32(mem0, x);
         mem0 = mem1 + y - SHL32(x,1);
         mem1 = x - SHR32(y,1);
         tmp[i] = SROUND16(y, 2);
         /*printf("%f ", tmp[i]);*/
      }
      /*printf("\n");*/
      /* First few samples are bad because we don't propagate the memory */
      OPUS_CLEAR(tmp, 12);

      /* Normalize tmp to max range */
      {
         int shift=0;
         shift = 14-celt_ilog2(MAX16(1, celt_maxabs16(tmp, len)));
         if (shift!=0)
         {
            for (i=0;i<len;i++)
               tmp[i] = SHL16(tmp[i], shift);
         }
      }

      mean=0;
      mem0=0;
      /* Grouping by two to reduce complexity */
      /* Forward pass to compute the post-echo threshold*/
      for (i=0;i<len2;i++)
      {
         opus_val32 x2 = PSHR32(MULT16_16(tmp[2*i],tmp[2*i]) + MULT16_16(tmp[2*i+1],tmp[2*i+1]),4);
         mean += PSHR32(x2, 12);
         /* FIXME: Use PSHR16() instead */
         mem0 = mem0 + PSHR32(x2-mem0,forward_shift);
         tmp[i] = PSHR32(mem0, 12);
      }

      mem0=0;
      maxE=0;
      /* Backward pass to compute the pre-echo threshold */
      for (i=len2-1;i>=0;i--)
      {
         /* Backward masking: 13.9 dB/ms. */
         /* FIXME: Use PSHR16() instead */
         mem0 = mem0 + PSHR32(SHL32(tmp[i],4)-mem0,3);
         tmp[i] = PSHR32(mem0, 4);
         maxE = MAX16(maxE, tmp[i]);
      }
      /*for (i=0;i<len2;i++)printf("%f ", tmp[i]/mean);printf("\n");*/

      /* Compute the ratio of the "frame energy" over the harmonic mean of the energy.
         This essentially corresponds to a bitrate-normalized temporal noise-to-mask
         ratio */

      /* As a compromise with the old transient detector, frame energy is the
         geometric mean of the energy and half the max */
      /* Costs two sqrt() to avoid overflows */
      mean = MULT16_16(celt_sqrt(mean), celt_sqrt(MULT16_16(maxE,len2>>1)));
      /* Inverse of the mean energy in Q15+6 */
      norm = SHL32(EXTEND32(len2),6+14)/ADD32(EPSILON,SHR32(mean,1));
      /* Compute harmonic mean discarding the unreliable boundaries
         The data is smooth, so we only take 1/4th of the samples */
      unmask=0;
      /* We should never see NaNs here. If we find any, then something really bad happened and we better abort
         before it does any damage later on. If these asserts are disabled (no hardening), then the table
         lookup a few lines below (id = ...) is likely to crash dur to an out-of-bounds read. DO NOT FIX
         that crash on NaN since it could result in a worse issue later on. */
      celt_assert(!celt_isnan(tmp[0]));
      celt_assert(!celt_isnan(norm));
      for (i=12;i<len2-5;i+=4)
      {
         int id;
         id = MAX32(0,MIN32(127,MULT16_32_Q15(tmp[i]+EPSILON,norm))); /* Do not round to nearest */
         unmask += inv_table[id];
      }
      /*printf("%d\n", unmask);*/
      /* Normalize, compensate for the 1/4th of the sample and the factor of 6 in the inverse table */
      unmask = 64*unmask*4/(6*(len2-17));
      if (unmask>mask_metric)
         mask_metric = unmask;
   }
   is_transient = mask_metric>200;
   /* Prevent the transient detector from confusing the partial cycle of a
      very low frequency tone with a transient. */
   if (toneishness > QCONST32(.98f, 29) && tone_freq < QCONST16(0.026f, 13))
   {
      is_transient = 0;
      mask_metric = 0;
   }
   /* For low bitrates, define "weak transients" that need to be
      handled differently to avoid partial collapse. */
   if (allow_weak_transients && is_transient && mask_metric<600) {
      is_transient = 0;
      *weak_transient = 1;
   }
   /* Arbitrary metric for VBR boost */
   tf_max = MAX16(0,celt_sqrt(27*mask_metric)-42);
   /* *tf_estimate = 1 + MIN16(1, sqrt(MAX16(0, tf_max-30))/20); */
   *tf_estimate = celt_sqrt(MAX32(0, SHL32(MULT16_16(QCONST16(0.0069,14),MIN16(163,tf_max)),14)-QCONST32(0.139,28)));
   /*printf("%d %f\n", tf_max, mask_metric);*/
   RESTORE_STACK;
   /*printf("%d %f %d\n", is_transient, (float)*tf_estimate, tf_max);*/
   return is_transient;
}

/** Apply window and compute the MDCT for all sub-frames and
    all channels in a frame */
static void compute_mdcts(const CELTMode *mode, int shortBlocks, celt_sig * OPUS_RESTRICT in,
                          celt_sig * OPUS_RESTRICT out, int C, int CC, int LM, char *scratch)
{
   const int overlap = mode->overlap;
   int N;
   int B;
   int shift;
   int i, b, c;
   if (shortBlocks)
   {
      B = shortBlocks;
      N = mode->shortMdctSize;
      shift = mode->maxLM;
   } else {
      B = 1;
      N = mode->shortMdctSize<<LM;
      shift = mode->maxLM-LM;
   }
   c=0; do {
      for (b=0;b<B;b++)
      {
         /* Interleaving the sub-frames while doing the MDCTs */
         clt_mdct_forward(&mode->mdct, in+c*(B*N+overlap)+b*N,
                          &out[b+c*N*B], mode->window, overlap, shift, B, scratch);
      }
   } while (++c<CC);
   if (CC==2&&C==1)
   {
      for (i=0;i<B*N;i++)
         out[i] = ADD32(HALF32(out[i]), HALF32(out[B*N+i]));
   }
}


/* 48 kHz input; clipping only applies to float input. */
static void celt_preemphasis(const opus_res * OPUS_RESTRICT pcmp, celt_sig * OPUS_RESTRICT inp,
                        int N, int CC, const opus_val16 *coef, celt_sig *mem)
{
   int i;
   opus_val16 coef0;
   celt_sig m;

   coef0 = coef[0];
   m = *mem;
   for (i=0;i<N;i++)
   {
      celt_sig x;
      x = RES2SIG(pcmp[CC*i]);
      /* Apply pre-emphasis */
      inp[i] = x - m;
      m = MULT16_32_Q15(coef0, x);
   }
   *mem = m;
}


static void tf_encode(int start, int end, int isTransient, int *tf_res, int LM, int tf_select, ec_enc *enc)
{
   int curr, i;
   int tf_select_rsv;
   int tf_changed;
   int logp;
   opus_uint32 budget;
   opus_uint32 tell;
   budget = enc->storage*8;
   tell = ec_tell(enc);
   logp = isTransient ? 2 : 4;
   /* Reserve space to code the tf_select decision. */
   tf_select_rsv = LM>0 && tell+logp+1 <= budget;
   budget -= tf_select_rsv;
   curr = tf_changed = 0;
   SHORT_LOOP
   for (i=start;i<end;i++)
   {
      if (tell+logp<=budget)
      {
         ec_enc_bit_logp(enc, tf_res[i] ^ curr, logp);
         tell = ec_tell(enc);
         curr = tf_res[i];
         tf_changed |= curr;
      }
      else
         tf_res[i] = curr;
      logp = isTransient ? 4 : 5;
   }
   /* Only code tf_select if it would actually make a difference. */
   if (tf_select_rsv &&
         tf_select_table[LM][4*isTransient+0+tf_changed]!=
         tf_select_table[LM][4*isTransient+2+tf_changed])
      ec_enc_bit_logp(enc, tf_select, 1);
   else
      tf_select = 0;
   SHORT_LOOP
   for (i=start;i<end;i++)
      tf_res[i] = tf_select_table[LM][4*isTransient+2*tf_select+tf_res[i]];
   /*for(i=0;i<end;i++)printf("%d ", isTransient ? tf_res[i] : LM+tf_res[i]);printf("\n");*/
}

static int stereo_analysis(const CELTMode *m, const celt_norm *X,
      int LM, int N0)
{
   int i;
   int thetas;
   opus_val32 sumLR = EPSILON, sumMS = EPSILON;

   /* Use the L1 norm to model the entropy of the L/R signal vs the M/S signal */
   for (i=0;i<13;i++)
   {
      int j;
      for (j=m->eBands[i]<<LM;j<m->eBands[i+1]<<LM;j++)
      {
         opus_val32 L, R, M, S;
         /* We cast to 32-bit first because of the -32768 case */
         L = SHR32(X[j], NORM_SHIFT-14);
         R = SHR32(X[N0+j], NORM_SHIFT-14);
         M = ADD32(L, R);
         S = SUB32(L, R);
         sumLR = ADD32(sumLR, ADD32(ABS32(L), ABS32(R)));
         sumMS = ADD32(sumMS, ADD32(ABS32(M), ABS32(S)));
      }
   }
   sumMS = MULT16_32_Q15(QCONST16(0.707107f, 15), sumMS);
   thetas = 13;
   /* We don't need thetas for lower bands with LM<=1 */
   if (LM<=1)
      thetas -= 8;
   return MULT16_32_Q15((m->eBands[13]<<(LM+1))+thetas, sumMS)
         > MULT16_32_Q15(m->eBands[13]<<(LM+1), sumLR);
}

#define MSWAP(a,b) do {celt_glog tmp = a;a=b;b=tmp;} while(0)
static celt_glog median_of_5(const celt_glog *x)
{
   celt_glog t0, t1, t2, t3, t4;
   t2 = x[2];
   if (x[0] > x[1])
   {
      t0 = x[1];
      t1 = x[0];
   } else {
      t0 = x[0];
      t1 = x[1];
   }
   if (x[3] > x[4])
   {
      t3 = x[4];
      t4 = x[3];
   } else {
      t3 = x[3];
      t4 = x[4];
   }
   if (t0 > t3)
   {
      MSWAP(t0, t3);
      MSWAP(t1, t4);
   }
   if (t2 > t1)
   {
      if (t1 < t3)
         return MING(t2, t3);
      else
         return MING(t4, t1);
   } else {
      if (t2 < t3)
         return MING(t1, t3);
      else
         return MING(t2, t4);
   }
}

static celt_glog median_of_3(const celt_glog *x)
{
   celt_glog t0, t1, t2;
   if (x[0] > x[1])
   {
      t0 = x[1];
      t1 = x[0];
   } else {
      t0 = x[0];
      t1 = x[1];
   }
   t2 = x[2];
   if (t1 < t2)
      return t1;
   else if (t0 < t2)
      return t2;
   else
      return t0;
}

/* Boosts of the high bands, in bit allocation units, from how far each band rises above the
   masking of its neighbours; `offsets` receives them. */
static void dynalloc_analysis(const celt_glog *bandLogE, const celt_glog *bandLogE2,
      int nbEBands, int start, int end, int C, int *offsets, const opus_int16 *logN,
      const opus_int16 *eBands, int LM, int effectiveBytes,
      opus_val16 tone_freq, opus_val32 toneishness, char *scratch)
{
   int i, c;
   VARDECL(celt_glog, follower);
   VARDECL(celt_glog, noise_floor);
   VARDECL(celt_glog, bandLogE3);
   SAVE_STACK;
   ALLOC(follower, C*nbEBands, celt_glog);
   ALLOC(noise_floor, C*nbEBands, celt_glog);
   ALLOC(bandLogE3, nbEBands, celt_glog);
   OPUS_CLEAR(offsets, nbEBands);
   /* Dynamic allocation code */
   SHORT_LOOP
   for (i=0;i<end;i++)
   {
      /* Noise floor must take into account eMeans, the depth, the width of the bands
         and the preemphasis filter (approx. square of bark band ID). The input has 16 significant bits. */
      noise_floor[i] = GCONST(0.0625f)*logN[i]
            +GCONST(.5f)+SHL32(9-16,DB_SHIFT)-SHL32(eMeans[i],DB_SHIFT-4)
            +GCONST(.0062f)*(i+5)*(i+5);
   }
   /* Make sure that dynamic allocation can't make us bust the budget.
      We enable the feature starting at 24 kb/s for 20-ms frames
      and 96 kb/s for 2.5 ms frames.  */
   if (effectiveBytes >= (30 + 5*LM))
   {
      int last=0;
      c=0;do
      {
         celt_glog offset;
         celt_glog tmp;
         celt_glog *f;
         OPUS_COPY(bandLogE3, &bandLogE2[c*nbEBands], end);
         f = &follower[c*nbEBands];
         f[0] = bandLogE3[0];
         SHORT_LOOP
         for (i=1;i<end;i++)
         {
            /* The last band to be at least 3 dB higher than the previous one
               is the last we'll consider. Otherwise, we run into problems on
               bandlimited signals. */
            if (bandLogE3[i] > bandLogE3[i-1]+GCONST(.5f))
               last=i;
            f[i] = MING(f[i-1]+GCONST(1.5f), bandLogE3[i]);
         }
         SHORT_LOOP
         for (i=last-1;i>=0;i--)
            f[i] = MING(f[i], MING(f[i+1]+GCONST(2.f), bandLogE3[i]));

         /* Combine with a median filter to avoid dynalloc triggering unnecessarily.
            The "offset" value controls how conservative we are -- a higher offset
            reduces the impact of the median filter and makes dynalloc use more bits. */
         offset = GCONST(1.f);
         for (i=2;i<end-2;i++)
            f[i] = MAXG(f[i], median_of_5(&bandLogE3[i-2])-offset);
         tmp = median_of_3(&bandLogE3[0])-offset;
         f[0] = MAXG(f[0], tmp);
         f[1] = MAXG(f[1], tmp);
         tmp = median_of_3(&bandLogE3[end-3])-offset;
         f[end-2] = MAXG(f[end-2], tmp);
         f[end-1] = MAXG(f[end-1], tmp);

         SHORT_LOOP
         for (i=0;i<end;i++)
            f[i] = MAXG(f[i], noise_floor[i]);
      } while (++c<C);
      if (C==2)
      {
         SHORT_LOOP
         for (i=start;i<end;i++)
         {
            /* Consider 24 dB "cross-talk" */
            follower[nbEBands+i] = MAXG(follower[nbEBands+i], follower[         i]-GCONST(4.f));
            follower[         i] = MAXG(follower[         i], follower[nbEBands+i]-GCONST(4.f));
            follower[i] = HALF32(MAXG(0, bandLogE[i]-follower[i]) + MAXG(0, bandLogE[nbEBands+i]-follower[nbEBands+i]));
         }
      } else {
         SHORT_LOOP
         for (i=start;i<end;i++)
         {
            follower[i] = MAXG(0, bandLogE[i]-follower[i]);
         }
      }
      for (i=start;i<end;i++)
      {
         if (i<8)
            follower[i] *= 2;
         if (i>=12)
            follower[i] = HALF32(follower[i]);
      }
      /* Compensate for Opus' under-allocation on tones. */
      if (toneishness > QCONST32(.98f, 29)) {
         int freq_bin = PSHR32(((opus_val32)tone_freq)*QCONST16(120/M_PI, 9), 13+9);
         for (i=start;i<end;i++) {
            if (freq_bin >= eBands[i] && freq_bin <= eBands[i+1]) follower[i] += GCONST(2.f);
            if (freq_bin >= eBands[i]-1 && freq_bin <= eBands[i+1]+1) follower[i] += GCONST(1.f);
            if (freq_bin >= eBands[i]-2 && freq_bin <= eBands[i+1]+2) follower[i] += GCONST(1.f);
            if (freq_bin >= eBands[i]-3 && freq_bin <= eBands[i+1]+3) follower[i] += GCONST(.5f);
         }
         if (freq_bin >= eBands[end]) {
            follower[end-1] += GCONST(2.f);
            follower[end-2] += GCONST(1.f);
         }
      }
      for (i=start;i<end;i++)
      {
         int width;
         int boost;

         follower[i] = MING(follower[i], GCONST(4));

         follower[i] = SHR32(follower[i], 8);
         width = C*(eBands[i+1]-eBands[i])<<LM;
         if (width<6)
         {
            boost = (int)SHR32(follower[i],DB_SHIFT-8);
         } else if (width > 48) {
            boost = (int)SHR32(follower[i]*8,DB_SHIFT-8);
         } else {
            boost = (int)SHR32(follower[i]*width/6,DB_SHIFT-8);
         }
         offsets[i] = boost;
      }
   }
   RESTORE_STACK;
}

void normalize_tone_input(opus_val16 *x, int len) {
   opus_val32 ac0=len;
   int i;
   int shift;
   for (i=0;i<len;i++) {
      ac0 = ADD32(ac0, SHR32(MULT16_16(x[i], x[i]), 10));
   }
   shift = 5 - (28-celt_ilog2(ac0))/2;
   if (shift > 0) {
      for (i=0;i<len;i++) {
         x[i] = PSHR32(x[i], shift);
      }
   }
}
int acos_approx(opus_val32 x) {
   opus_val16 x14;
   opus_val32 tmp;
   int flip = x<0;
   x = abs(x);
   x14 = x>>15;
   tmp = (762*x14>>14)-3308;
   tmp = (tmp*x14>>14)+25726;
   tmp = tmp*celt_sqrt(IMAX(0, (1<<30) - (x<<1)))>>16;
   if (flip) tmp = 25736 - tmp;
   return tmp;
}

/* Compute the LPC coefficients using a least-squares fit for both forward and backward prediction. */
static int tone_lpc(const opus_val16 *x, int len, int delay, opus_val32 *lpc) {
   int i;
   opus_val32 r00=0, r01=0, r11=0, r02=0, r12=0, r22=0;
   opus_val32 edges;
   opus_val32 num0, num1, den;
   celt_assert(len > 2*delay);
   /* Compute correlations as if using the forward prediction covariance method. */
   for (i=0;i<len-2*delay;i++) {
      r00 += MULT16_16(x[i],x[i]);
      r01 += MULT16_16(x[i],x[i+delay]);
      r02 += MULT16_16(x[i],x[i+2*delay]);
   }
   edges = 0;
   SHORT_LOOP
   for (i=0;i<delay;i++) edges += MULT16_16(x[len+i-2*delay],x[len+i-2*delay]) - MULT16_16(x[i],x[i]);
   r11 = r00+edges;
   edges = 0;
   SHORT_LOOP
   for (i=0;i<delay;i++) edges += MULT16_16(x[len+i-delay],x[len+i-delay]) - MULT16_16(x[i+delay],x[i+delay]);
   r22 = r11+edges;
   edges = 0;
   SHORT_LOOP
   for (i=0;i<delay;i++) edges += MULT16_16(x[len+i-2*delay],x[len+i-delay]) - MULT16_16(x[i],x[i+delay]);
   r12 = r01+edges;
   /* Reverse and sum to get the backward contribution. */
   {
      opus_val32 R00, R01, R11, R02, R12, R22;
      R00 = r00 + r22;
      R01 = r01 + r12;
      R11 = 2*r11;
      R02 = 2*r02;
      R12 = r12 + r01;
      R22 = r00 + r22;
      r00 = R00;
      r01 = R01;
      r11 = R11;
      r02 = R02;
      r12 = R12;
      r22 = R22;
   }
   /* Solve A*x=b, where A=[r00, r01; r01, r11] and b=[r02; r12]. */
   den = MULT32_32_Q31(r00,r11) - MULT32_32_Q31(r01,r01);
   if (den <= SHR32(MULT32_32_Q31(r00,r11), 10)) return 1;
   num1 = MULT32_32_Q31(r02,r11) - MULT32_32_Q31(r01,r12);
   if (num1 >= den) lpc[1] = QCONST32(1.f, 29);
   else if (num1 <= -den) lpc[1] = -QCONST32(1.f, 29);
   else lpc[1] = frac_div32_q29(num1, den);
   num0 = MULT32_32_Q31(r00,r12) - MULT32_32_Q31(r02,r01);
   if (HALF32(num0) >= den) lpc[0] = QCONST32(1.999999f, 29);
   else if (HALF32(num0) <= -den) lpc[0] = -QCONST32(1.999999f, 29);
   else lpc[0] = frac_div32_q29(num0, den);
   /*printf("%f %f\n", lpc[0], lpc[1]);*/
   return 0;
}

/* Detects pure of nearly pure tones so we can prevent them from causing problems with the encoder. */
static opus_val16 tone_detect(const celt_sig *in, int CC, int N, opus_val32 *toneishness, opus_int32 Fs, char *scratch) {
   int i;
   int delay = 1;
   int fail;
   opus_val32 lpc[2];
   opus_val16 freq;
   VARDECL(opus_val16, x);
   SAVE_STACK;
   ALLOC(x, N, opus_val16);
   /* Shift by SIG_SHIFT+2 (+3 for stereo) to account for HF gain of the preemphasis filter. */
   if (CC==2) {
      for (i=0;i<N;i++) x[i] = PSHR32(ADD32(SHR32(in[i], 1), SHR32(in[i+N], 1)), SIG_SHIFT+2);
   } else {
      for (i=0;i<N;i++) x[i] = PSHR32(in[i], SIG_SHIFT+2);
   }
   normalize_tone_input(x, N);
   fail = tone_lpc(x, N, delay, lpc);
   /* If our LPC filter resonates too close to DC, retry the analysis with down-sampling. */
   while (delay <= Fs/3000 && (fail || (lpc[0] > QCONST32(1.f, 29) && lpc[1] < 0))) {
      delay *= 2;
      fail = tone_lpc(x, N, delay, lpc);
   }
   /* Check that our filter has complex roots. */
   if (!fail && MULT32_32_Q31(lpc[0],lpc[0]) + MULT32_32_Q31(QCONST32(3.999999, 29), lpc[1]) < 0) {
      /* Squared radius of the poles. */
      *toneishness = -lpc[1];
      freq = (acos_approx(lpc[0]>>1)+delay/2)/delay;
   } else {
      freq = -1;
      *toneishness=0;
   }
   /*printf("%f %f %f %f\n", freq, lpc[0], lpc[1], *toneishness);*/
   RESTORE_STACK;
   return freq;
}

/* Codes bands 17 to 20 of one 20 ms hybrid frame into `enc`, after the SILK layer, with the VBR target the
   SILK layer leaves (`st->bitrate`). Returns the number of bytes the frame occupies. */
int celt_encode_with_ec(CELTEncoder * OPUS_RESTRICT st, const opus_res * pcm, int nbCompressedBytes, ec_enc *enc, char *scratch)
{
   int i, c;
   opus_int32 bits;
   VARDECL(celt_sig, in);
   VARDECL(celt_sig, freq);
   VARDECL(celt_norm, X);
   VARDECL(celt_ener, bandE);
   VARDECL(celt_glog, bandLogE);
   VARDECL(celt_glog, bandLogE2);
   VARDECL(int, fine_quant);
   VARDECL(celt_glog, error);
   VARDECL(int, pulses);
   VARDECL(int, cap);
   VARDECL(int, offsets);
   VARDECL(int, fine_priority);
   VARDECL(int, tf_res);
   VARDECL(unsigned char, collapse_masks);
   celt_glog *oldBandE, *oldLogE, *oldLogE2, *energyError;
   int shortBlocks=0;
   int isTransient=0;
   const int CC = st->channels;
   const int C = st->stream_channels;
   const int LM = CELT_FRAME_LM;
   const int M = 1<<LM;
   const int N = CELT_FRAME_SIZE;
   const int start = CELT_HYBRID_START;
   int tf_select;
   int spread_decision;
   int nbFilledBytes;
   opus_int32 min_allowed;
   int end;
   int codedBands;
   int alloc_trim;
   int dual_stereo=0;
   int effectiveBytes;
   int dynalloc_logp;
   opus_int32 vbr_rate;
   opus_int32 total_bits;
   opus_int32 total_boost;
   opus_int32 balance;
   opus_int32 tell;
   opus_int32 tell0_frac;
   int anti_collapse_rsv;
   int anti_collapse_on=0;
   opus_val16 tf_estimate;
   const OpusCustomMode *mode;
   int nbEBands;
   int overlap;
   const opus_int16 *eBands;
   int secondMdct;
   int signalBandwidth;
   int transient_coded;
   int transient_got_disabled=0;
   opus_int32 equiv_rate;
   int weak_transient = 0;
   opus_val16 tone_freq=-1;
   opus_val32 toneishness=0;
   ALLOC_STACK;

   mode = st->mode;
   nbEBands = mode->nbEBands;
   overlap = mode->overlap;
   eBands = mode->eBands;
   end = mode->effEBands;
   tf_estimate = 0;

   oldBandE = (celt_glog*)(st->in_mem+CC*overlap);
   oldLogE = oldBandE + CC*nbEBands;
   oldLogE2 = oldLogE + CC*nbEBands;
   energyError = oldLogE2 + CC*nbEBands;

   tell0_frac=ec_tell_frac(enc);
   tell=ec_tell(enc);
   nbFilledBytes=(tell+4)>>3;

   vbr_rate = bitrate_to_bits(st->bitrate, mode->Fs, N)<<BITRES;
   effectiveBytes = vbr_rate>>(3+BITRES);
   equiv_rate = ((opus_int32)nbCompressedBytes*8*50 << (3-LM)) - (40*C+20)*((400>>LM) - 50);
   equiv_rate = IMIN(equiv_rate, st->bitrate - (40*C+20)*((400>>LM) - 50));

   total_bits = nbCompressedBytes*8;

   ALLOC(in, CC*(N+overlap), celt_sig);

   /* The SILK layer has coded bits already, so the frame is never a silent one. */
   c=0; do {
      celt_preemphasis(pcm+c, in+c*(N+overlap)+overlap, N, CC,
                  mode->preemph, st->preemph_memE+c);
      OPUS_COPY(in+c*(N+overlap), st->in_mem+c*overlap, overlap);
      OPUS_COPY(st->in_mem+c*overlap, in+c*(N+overlap)+N, overlap);
   } while (++c<CC);

   tone_freq = tone_detect(in, CC, N+overlap, &toneishness, mode->Fs, scratch);
   {
      /* Reduces the likelihood of energy instability on fricatives at low bitrate
         in hybrid mode. It seems like we still want to have real transients on vowels
         though (small SILK quantization offset value). */
      int allow_weak_transients = effectiveBytes<15 && st->silk_info.signalType != 2;
      isTransient = transient_analysis(in, N+overlap, CC,
            &tf_estimate, allow_weak_transients, &weak_transient, tone_freq, toneishness, scratch);
   }
   toneishness = MIN32(toneishness, QCONST32(1.f, 29)-SHL32(tf_estimate, 15));
   transient_coded = ec_tell(enc)+3<=total_bits;
   if (transient_coded)
   {
      if (isTransient)
         shortBlocks = M;
   } else {
      isTransient = 0;
      transient_got_disabled=1;
   }

   ALLOC(freq, CC*N, celt_sig); /**< Interleaved signal MDCTs */
   ALLOC(bandE,nbEBands*CC, celt_ener);
   ALLOC(bandLogE,nbEBands*CC, celt_glog);

   secondMdct = shortBlocks && st->complexity>=8;
   ALLOC(bandLogE2, C*nbEBands, celt_glog);
   if (secondMdct)
   {
      compute_mdcts(mode, 0, in, freq, C, CC, LM, scratch);
      compute_band_energies(mode, freq, bandE, end, C, LM);
      amp2Log2(mode, end, end, bandE, bandLogE2, C);
      for (c=0;c<C;c++)
      {
         SHORT_LOOP
         for (i=0;i<end;i++)
            bandLogE2[nbEBands*c+i] += HALF32(SHL32(LM, DB_SHIFT));
      }
   }

   compute_mdcts(mode, shortBlocks, in, freq, C, CC, LM, scratch);
   /* This should catch any NaN in the CELT input. Since we're not supposed to see any (they're filtered
      at the Opus layer), just abort. */
   celt_assert(!celt_isnan(freq[0]) && (C==1 || !celt_isnan(freq[N])));
   compute_band_energies(mode, freq, bandE, end, C, LM);

   amp2Log2(mode, end, end, bandE, bandLogE, C);

   if (!secondMdct)
   {
      OPUS_COPY(bandLogE2, bandLogE, C*nbEBands);
   }

   if (transient_coded)
      ec_enc_bit_logp(enc, isTransient, 3);

   ALLOC(X, C*N, celt_norm);         /**< Interleaved normalised MDCTs */

   /* Band normalisation */
   normalise_bands(mode, freq, X, bandE, end, C, M);

   ALLOC(offsets, nbEBands, int);

   dynalloc_analysis(bandLogE, bandLogE2, nbEBands, start, end, C, offsets,
         mode->logN, eBands, LM, effectiveBytes, tone_freq, toneishness, scratch);

   ALLOC(tf_res, nbEBands, int);
   /* No tf analysis for hybrid frames */
   if (weak_transient)
   {
      /* For weak transients, we rely on the fact that improving time resolution using
         TF on a long window is imperfect and will not result in an energy collapse at
         low bitrate. */
      SHORT_LOOP
      for (i=0;i<end;i++)
         tf_res[i] = 1;
      tf_select=0;
   } else if (effectiveBytes<15 && st->silk_info.signalType != 2)
   {
      /* For low bitrate hybrid, we force temporal resolution to 5 ms rather than 2.5 ms. */
      SHORT_LOOP
      for (i=0;i<end;i++)
         tf_res[i] = 0;
      tf_select=isTransient;
   } else {
      SHORT_LOOP
      for (i=0;i<end;i++)
         tf_res[i] = isTransient;
      tf_select=0;
   }

   ALLOC(error, C*nbEBands, celt_glog);
   c=0;
   do {
      SHORT_LOOP
      for (i=start;i<end;i++)
      {
         /* When the energy is stable, slightly bias energy quantization towards
            the previous error to make the gain more stable (a constant offset is
            better than fluctuations). */
         if (ABS32(SUB32(bandLogE[i+c*nbEBands], oldBandE[i+c*nbEBands])) < GCONST(2.f))
         {
            bandLogE[i+c*nbEBands] -= MULT16_32_Q15(QCONST16(0.25f, 15), energyError[i+c*nbEBands]);
         }
      }
   } while (++c < C);
   quant_coarse_energy(mode, start, end, end, bandLogE,
         oldBandE, total_bits, error, enc,
         C, LM, nbCompressedBytes-nbFilledBytes, 0,
         &st->delayedIntra, st->complexity >= 4, st->loss_rate, scratch);

   tf_encode(start, end, isTransient, tf_res, LM, tf_select, enc);

   if (ec_tell(enc)+4<=total_bits)
   {
      spread_decision = isTransient ? SPREAD_NORMAL : SPREAD_AGGRESSIVE;
      ec_enc_icdf(enc, spread_decision, spread_icdf, 5);
   } else {
      spread_decision = SPREAD_NORMAL;
   }

   ALLOC(cap, nbEBands, int);
   init_caps(mode,cap,LM,C);

   dynalloc_logp = 6;
   total_bits<<=BITRES;
   total_boost = 0;
   tell = ec_tell_frac(enc);
   for (i=start;i<end;i++)
   {
      int width, quanta;
      int dynalloc_loop_logp;
      int boost;
      int j;
      width = C*(eBands[i+1]-eBands[i])<<LM;
      /* quanta is 6 bits, but no more than 1 bit/sample
         and no less than 1/8 bit/sample */
      quanta = IMIN(width<<BITRES, IMAX(6<<BITRES, width));
      dynalloc_loop_logp = dynalloc_logp;
      boost = 0;
      for (j = 0; tell+(dynalloc_loop_logp<<BITRES) < total_bits-total_boost
            && boost < cap[i]; j++)
      {
         int flag;
         flag = j<offsets[i];
         ec_enc_bit_logp(enc, flag, dynalloc_loop_logp);
         tell = ec_tell_frac(enc);
         if (!flag)
            break;
         boost += quanta;
         total_boost += quanta;
         dynalloc_loop_logp = 1;
      }
      /* Making dynalloc more likely */
      if (j)
         dynalloc_logp = IMAX(2, dynalloc_logp-1);
      offsets[i] = boost;
   }

   if (C==2)
   {
      static const opus_val16 intensity_thresholds[21]=
      /* 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 19  20  off*/
        {  1, 2, 3, 4, 5, 6, 7, 8,16,24,36,44,50,56,62,67,72,79,88,106,134};
      static const opus_val16 intensity_histeresis[21]=
        {  1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 4, 5, 6,  8, 8};

      dual_stereo = stereo_analysis(mode, X, LM, N);

      st->intensity = hysteresis_decision((opus_val16)(equiv_rate/1000),
            intensity_thresholds, intensity_histeresis, 21, st->intensity);
      st->intensity = IMIN(end,IMAX(start, st->intensity));
   }

   alloc_trim = 5;
   if (tell+(6<<BITRES) <= total_bits - total_boost)
   {
      ec_enc_icdf(enc, alloc_trim, trim_icdf, 7);
      tell = ec_tell_frac(enc);
   }

   /* In VBR mode the frame size must not be reduced so much that it would
       result in the encoder running out of bits.
      The margin of 2 bytes ensures that none of the bust-prevention logic
       in the decoder will have triggered so far. */
   min_allowed = ((tell+total_boost+(1<<(BITRES+3))-1)>>(BITRES+3)) + 2;
   /* Take into account the 37 bits we need to have left in the packet to
      signal a redundant frame in hybrid mode. Creating a shorter packet would
      create an entropy coder desync. */
   min_allowed = IMAX(min_allowed, (tell0_frac+(37<<BITRES)+total_boost+(1<<(BITRES+3))-1)>>(BITRES+3));
   /* Variable bitrate */
   {
     /* The target rate in 8th bits per frame */
     opus_int32 target;

     target = IMAX(0, vbr_rate - ((9*C+4)<<BITRES));
     /* Tonal frames (offset<100) need more bits than noisy (offset>100) ones. */
     if (st->silk_info.offset < 100) target += 12 << BITRES >> (3-LM);
     if (st->silk_info.offset > 100) target -= 18 << BITRES >> (3-LM);
     /* Boosting bitrate on transients and vowels with significant temporal
        spikes. */
     target += (opus_int32)MULT16_16_Q14(tf_estimate-QCONST16(.25f,14), (50<<BITRES));
     /* If we have a strong transient, let's make sure it has enough bits to code
        the first two bands, so that it can use folding rather than noise. */
     if (tf_estimate > QCONST16(.7f,14))
        target = IMAX(target, 50<<BITRES);
     /* The current offset is removed from the target and the space used
        so far is added*/
     target=target+tell;

     nbCompressedBytes = IMIN(nbCompressedBytes, IMAX(min_allowed, (target+(1<<(BITRES+2)))>>(BITRES+3)));
     /* This moves the raw bits to take into account the new compressed size */
     ec_enc_shrink(enc, nbCompressedBytes);
   }

   /* Bit allocation */
   ALLOC(fine_quant, nbEBands, int);
   ALLOC(pulses, nbEBands, int);
   ALLOC(fine_priority, nbEBands, int);

   /* bits =           packet size                    - where we are - safety*/
   bits = (((opus_int32)nbCompressedBytes*8)<<BITRES) - (opus_int32)ec_tell_frac(enc) - 1;
   anti_collapse_rsv = isTransient&&LM>=2&&bits>=((LM+2)<<BITRES) ? (1<<BITRES) : 0;
   bits -= anti_collapse_rsv;
   signalBandwidth = end-1;
   codedBands = clt_compute_allocation(mode, start, end, offsets, cap,
         alloc_trim, &st->intensity, &dual_stereo, bits, &balance, pulses,
         fine_quant, fine_priority, C, LM, enc, 1, st->lastCodedBands, signalBandwidth, scratch);
   if (st->lastCodedBands)
      st->lastCodedBands = IMIN(st->lastCodedBands+1,IMAX(st->lastCodedBands-1,codedBands));
   else
      st->lastCodedBands = codedBands;

   quant_fine_energy(mode, start, end, oldBandE, error, NULL, fine_quant, enc, C);
   OPUS_CLEAR(energyError, nbEBands*CC);

   /* Residual quantisation */
   ALLOC(collapse_masks, C*nbEBands, unsigned char);
   quant_all_bands(1, mode, start, end, X, C==2 ? X+N : NULL, collapse_masks,
         bandE, pulses, shortBlocks, spread_decision,
         dual_stereo, st->intensity, tf_res, nbCompressedBytes*(8<<BITRES)-anti_collapse_rsv,
         balance, enc, LM, codedBands, &st->rng, st->complexity, 0, scratch);


   if (anti_collapse_rsv > 0)
   {
      anti_collapse_on = st->consec_transient<2;
      ec_enc_bits(enc, anti_collapse_on, 1);
   }
   quant_energy_finalise(mode, start, end, oldBandE, error, fine_quant, fine_priority, nbCompressedBytes*8-ec_tell(enc), enc, C);
   c=0;
   do {
      SHORT_LOOP
      for (i=start;i<end;i++)
      {
         energyError[i+c*nbEBands] = MAXG(-GCONST(0.5f), MING(GCONST(0.5f), error[i+c*nbEBands]));
      }
   } while (++c < C);

   if (CC==2&&C==1) {
      OPUS_COPY(&oldBandE[nbEBands], oldBandE, nbEBands);
   }

   if (!isTransient)
   {
      OPUS_COPY(oldLogE2, oldLogE, CC*nbEBands);
      OPUS_COPY(oldLogE, oldBandE, CC*nbEBands);
   } else {
      SHORT_LOOP
      for (i=0;i<CC*nbEBands;i++)
         oldLogE[i] = MING(oldLogE[i], oldBandE[i]);
   }
   /* The bands below `start` are not coded */
   c=0; do
   {
      SHORT_LOOP
      for (i=0;i<start;i++)
      {
         oldBandE[c*nbEBands+i]=0;
         oldLogE[c*nbEBands+i]=oldLogE2[c*nbEBands+i]=-GCONST(28.f);
      }
      SHORT_LOOP
      for (i=end;i<nbEBands;i++)
      {
         oldBandE[c*nbEBands+i]=0;
         oldLogE[c*nbEBands+i]=oldLogE2[c*nbEBands+i]=-GCONST(28.f);
      }
   } while (++c<CC);

   if (isTransient || transient_got_disabled)
      st->consec_transient++;
   else
      st->consec_transient=0;
   st->rng = enc->rng;

   /* If there's any room left (can only happen for very high rates),
      it's already filled with zeros */
   ec_enc_done(enc);

   RESTORE_STACK;
   if (ec_get_error(enc))
      return OPUS_INTERNAL_ERROR;
   else
      return nbCompressedBytes;
}


int opus_custom_encoder_ctl(CELTEncoder * OPUS_RESTRICT st, int request, ...)
{
   va_list ap;

   va_start(ap, request);
   switch (request)
   {
      case OPUS_SET_COMPLEXITY_REQUEST:
      {
         int value = va_arg(ap, opus_int32);
         if (value<0 || value>10)
            goto bad_arg;
         st->complexity = value;
      }
      break;
      case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
      {
         int value = va_arg(ap, opus_int32);
         if (value<0 || value>100)
            goto bad_arg;
         st->loss_rate = value;
      }
      break;
      case OPUS_SET_BITRATE_REQUEST:
      {
         opus_int32 value = va_arg(ap, opus_int32);
         if (value<=500)
            goto bad_arg;
         value = IMIN(value, 750000*st->channels);
         st->bitrate = value;
      }
      break;
      case CELT_SET_CHANNELS_REQUEST:
      {
         opus_int32 value = va_arg(ap, opus_int32);
         if (value<1 || value>2)
            goto bad_arg;
         st->stream_channels = value;
      }
      break;
      case CELT_SET_SILK_INFO_REQUEST:
      {
         SILKInfo *info = va_arg(ap, SILKInfo *);
         if (info)
            OPUS_COPY(&st->silk_info, info, 1);
      }
      break;
      case CELT_GET_MODE_REQUEST:
      {
         const CELTMode ** value = va_arg(ap, const CELTMode**);
         if (value==0)
            goto bad_arg;
         *value=st->mode;
      }
      break;
      default:
         goto bad_request;
   }
   va_end(ap);
   return OPUS_OK;
bad_arg:
   va_end(ap);
   return OPUS_BAD_ARG;
bad_request:
   va_end(ap);
   return OPUS_UNIMPLEMENTED;
}
