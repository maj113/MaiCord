/*Copyright (c) 2003-2004, Mark Borgerding
  Lots of modifications by Jean-Marc Valin
  Copyright (c) 2005-2007, Xiph.Org Foundation
  Copyright (c) 2008,      Xiph.Org Foundation, CSIRO

  All rights reserved.

  Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.*/

/* This code is originally from Mark Borgerding's KISS-FFT but has been
   heavily modified to better suit Opus */


#include "_kiss_fft_guts.h"
#include "arch.h"
#include "os_support.h"
#include "mathops.h"
#include "stack_alloc.h"

#include "x86/x86cpu.h"
/* maiopus: the radix-2/3/4/5 butterflies two complex values at a time. A
   vector holds [r0, i0, r1, i1]; products are MULT16_32_Q15 per lane and sums
   wrap like the _ovflw macros, so the results are bit-exact with upstream's
   scalar butterflies. */
#define KF_LOAD(p) _mm_loadu_si128((__m128i *)(void*)(p))
#define KF_STORE(p, v) _mm_storeu_si128((__m128i *)(void*)(p), v)
/* Lanes 1 and 3 (the imaginary parts) from b, the real parts from a. */
#define KF_BLEND_IMAG(a, b) _mm_blend_epi16(a, b, 0xCC)
/* [r, i] -> [i, r] in both halves. */
#define KF_SWAP(v) _mm_shuffle_epi32(v, _MM_SHUFFLE(2,3,0,1))

static OPUS_INLINE __m128i kf_twiddles(const kiss_twiddle_cpx *a, const kiss_twiddle_cpx *b)
{
   return _mm_cvtepi16_epi32(_mm_unpacklo_epi32(_mm_loadu_si32(a), _mm_loadu_si32(b)));
}

/* C_MUL() on two complex values. */
static OPUS_INLINE __m128i kf_cmul(__m128i a, __m128i tw)
{
   __m128i p = mult16_32_q15_sse4_1(a, _mm_shuffle_epi32(tw, _MM_SHUFFLE(2,2,0,0)));
   __m128i q = mult16_32_q15_sse4_1(KF_SWAP(a), _mm_shuffle_epi32(tw, _MM_SHUFFLE(3,3,1,1)));
   return KF_BLEND_IMAG(_mm_sub_epi32(p, q), _mm_add_epi32(p, q));
}

/* [r, i] -> [i, -r] in both halves. */
static OPUS_INLINE __m128i kf_swap_negate_imag(__m128i v)
{
   __m128i s = KF_SWAP(v);
   return KF_BLEND_IMAG(s, _mm_sub_epi32(_mm_setzero_si128(), s));
}

#ifndef M_PI
#define M_PI 3.141592653
#endif

/* The guts header contains all the multiplication and addition macros that are defined for
   complex numbers.  It also declares the kf_ internal functions.
*/

/* maiopus: SSE4.1 only. The 48 kHz mode's four FFTs (480, 240, 120 and 60
   points, factors in static_modes_fixed.h) run radix 4 with m == 1 first,
   then radix 2 with m == 4, radix 4 with m == 4 or 8, radix 3 with m == 4..32
   and radix 5 with m == 12..96: every m > 1 is even, so the pair loops below
   cover all butterflies. */
static void kf_bfly2(
                     kiss_fft_cpx * Fout,
                     int m,
                     int N
                    )
{
   int i;
   const __m128i vtw = _mm_set1_epi32(QCONST32(0.7071067812f, COEF_SHIFT-1));
   (void)m;
   /* We know that m==4 here because the radix-2 is just after a radix-4 */
   celt_assert(m==4);
   for (i=0;i<N;i++)
   {
      __m128i a = KF_LOAD(Fout), b = KF_LOAD(Fout+2);
      __m128i p = KF_LOAD(Fout+4), q = KF_LOAD(Fout+6);
      __m128i sp = KF_SWAP(p), sq = KF_SWAP(q);
      /* t for Fout2[0] and Fout2[1], then for Fout2[2] and Fout2[3]. */
      __m128i t01 = _mm_blend_epi16(p, mult16_32_q15_sse4_1(
            KF_BLEND_IMAG(_mm_add_epi32(p, sp), _mm_sub_epi32(p, sp)), vtw), 0xF0);
      __m128i t23 = _mm_blend_epi16(kf_swap_negate_imag(q), mult16_32_q15_sse4_1(
            KF_BLEND_IMAG(_mm_sub_epi32(sq, q), _mm_sub_epi32(_mm_sub_epi32(_mm_setzero_si128(), q), sq)), vtw), 0xF0);
      KF_STORE(Fout+4, _mm_sub_epi32(a, t01));
      KF_STORE(Fout, _mm_add_epi32(a, t01));
      KF_STORE(Fout+6, _mm_sub_epi32(b, t23));
      KF_STORE(Fout+2, _mm_add_epi32(b, t23));
      Fout += 8;
   }
}

static void kf_bfly4(
                     kiss_fft_cpx * Fout,
                     const size_t fstride,
                     const kiss_fft_state *st,
                     int m,
                     int N,
                     int mm
                    )
{
   int i;

   if (m==1)
   {
      /* Degenerate case where all the twiddles are 1. */
      for (i=0;i<N;i++)
      {
         __m128i a = KF_LOAD(Fout), b = KF_LOAD(Fout+2);
         /* [Fout[0]+Fout[2], Fout[1]+Fout[3]] and the differences. */
         __m128i sum = _mm_add_epi32(a, b), dif = _mm_sub_epi32(a, b);
         __m128i lo = _mm_unpacklo_epi64(sum, sum), hi = _mm_unpackhi_epi64(sum, sum);
         __m128i even = _mm_blend_epi16(_mm_add_epi32(lo, hi), _mm_sub_epi32(lo, hi), 0xF0);
         __m128i d0 = _mm_unpacklo_epi64(dif, dif), d1 = KF_SWAP(_mm_unpackhi_epi64(dif, dif));
         __m128i odd = _mm_blend_epi16(_mm_add_epi32(d0, d1), _mm_sub_epi32(d0, d1), 0x3C);
         KF_STORE(Fout, _mm_unpacklo_epi64(even, odd));
         KF_STORE(Fout+2, _mm_unpackhi_epi64(even, odd));
         Fout+=4;
      }
   } else {
      int j;
      const kiss_twiddle_cpx *tw1,*tw2,*tw3;
      const int m2=2*m;
      const int m3=3*m;
      kiss_fft_cpx * Fout_beg = Fout;
      for (i=0;i<N;i++)
      {
         Fout = Fout_beg + i*mm;
         tw3 = tw2 = tw1 = st->twiddles;
         for (j=0;j<m;j+=2)
         {
            __m128i f0 = KF_LOAD(Fout);
            __m128i s0 = kf_cmul(KF_LOAD(Fout+m), kf_twiddles(tw1, tw1+fstride));
            __m128i s1 = kf_cmul(KF_LOAD(Fout+m2), kf_twiddles(tw2, tw2+fstride*2));
            __m128i s2 = kf_cmul(KF_LOAD(Fout+m3), kf_twiddles(tw3, tw3+fstride*3));
            __m128i s5 = _mm_sub_epi32(f0, s1);
            __m128i s3 = _mm_add_epi32(s0, s2);
            __m128i s4 = KF_SWAP(_mm_sub_epi32(s0, s2));
            f0 = _mm_add_epi32(f0, s1);
            KF_STORE(Fout+m2, _mm_sub_epi32(f0, s3));
            KF_STORE(Fout, _mm_add_epi32(f0, s3));
            KF_STORE(Fout+m, KF_BLEND_IMAG(_mm_add_epi32(s5, s4), _mm_sub_epi32(s5, s4)));
            KF_STORE(Fout+m3, KF_BLEND_IMAG(_mm_sub_epi32(s5, s4), _mm_add_epi32(s5, s4)));
            tw1 += fstride*2;
            tw2 += fstride*4;
            tw3 += fstride*6;
            Fout += 2;
         }
      }
   }
}

static void kf_bfly3(
                     kiss_fft_cpx * Fout,
                     const size_t fstride,
                     const kiss_fft_state *st,
                     int m,
                     int N,
                     int mm
                    )
{
   int i, k;
   const size_t m2 = 2*m;
   const kiss_twiddle_cpx *tw1,*tw2;
   const __m128i epi3 = _mm_set1_epi32(-QCONST32(0.86602540f, COEF_SHIFT-1));
   kiss_fft_cpx * Fout_beg = Fout;
   for (i=0;i<N;i++)
   {
      Fout = Fout_beg + i*mm;
      tw1=tw2=st->twiddles;
      for (k=0;k<m;k+=2)
      {
         __m128i f0 = KF_LOAD(Fout);
         __m128i s1 = kf_cmul(KF_LOAD(Fout+m), kf_twiddles(tw1, tw1+fstride));
         __m128i s2 = kf_cmul(KF_LOAD(Fout+m2), kf_twiddles(tw2, tw2+fstride*2));
         __m128i s3 = _mm_add_epi32(s1, s2);
         __m128i s0 = KF_SWAP(mult16_32_q15_sse4_1(_mm_sub_epi32(s1, s2), epi3));
         __m128i fm = _mm_sub_epi32(f0, _mm_srai_epi32(s3, 1));
         KF_STORE(Fout, _mm_add_epi32(f0, s3));
         KF_STORE(Fout+m2, KF_BLEND_IMAG(_mm_add_epi32(fm, s0), _mm_sub_epi32(fm, s0)));
         KF_STORE(Fout+m, KF_BLEND_IMAG(_mm_sub_epi32(fm, s0), _mm_add_epi32(fm, s0)));
         tw1 += fstride*2;
         tw2 += fstride*4;
         Fout += 2;
      }
   }
}

static void kf_bfly5(
                     kiss_fft_cpx * Fout,
                     const size_t fstride,
                     const kiss_fft_state *st,
                     int m,
                     int N,
                     int mm
                    )
{
   kiss_fft_cpx *Fout0,*Fout1,*Fout2,*Fout3,*Fout4;
   int i, u;
   const kiss_twiddle_cpx *tw = st->twiddles;
   const __m128i yar = _mm_set1_epi32(QCONST32(0.30901699f, COEF_SHIFT-1));
   const __m128i yai = _mm_set1_epi32(-QCONST32(0.95105652f, COEF_SHIFT-1));
   const __m128i ybr = _mm_set1_epi32(-QCONST32(0.80901699f, COEF_SHIFT-1));
   const __m128i ybi = _mm_set1_epi32(-QCONST32(0.58778525f, COEF_SHIFT-1));
   kiss_fft_cpx * Fout_beg = Fout;

   for (i=0;i<N;i++)
   {
      Fout = Fout_beg + i*mm;
      Fout0=Fout;
      Fout1=Fout0+m;
      Fout2=Fout0+2*m;
      Fout3=Fout0+3*m;
      Fout4=Fout0+4*m;

      for (u=0;u<m;u+=2)
      {
         __m128i s0 = KF_LOAD(Fout0);
         __m128i s1 = kf_cmul(KF_LOAD(Fout1), kf_twiddles(&tw[u*fstride], &tw[(u+1)*fstride]));
         __m128i s2 = kf_cmul(KF_LOAD(Fout2), kf_twiddles(&tw[2*u*fstride], &tw[2*(u+1)*fstride]));
         __m128i s3 = kf_cmul(KF_LOAD(Fout3), kf_twiddles(&tw[3*u*fstride], &tw[3*(u+1)*fstride]));
         __m128i s4 = kf_cmul(KF_LOAD(Fout4), kf_twiddles(&tw[4*u*fstride], &tw[4*(u+1)*fstride]));
         __m128i s7 = _mm_add_epi32(s1, s4), s10 = _mm_sub_epi32(s1, s4);
         __m128i s8 = _mm_add_epi32(s2, s3), s9 = _mm_sub_epi32(s2, s3);
         __m128i s5 = _mm_add_epi32(s0, _mm_add_epi32(mult16_32_q15_sse4_1(s7, yar), mult16_32_q15_sse4_1(s8, ybr)));
         __m128i s6 = kf_swap_negate_imag(_mm_add_epi32(mult16_32_q15_sse4_1(s10, yai), mult16_32_q15_sse4_1(s9, ybi)));
         __m128i s11 = _mm_add_epi32(s0, _mm_add_epi32(mult16_32_q15_sse4_1(s7, ybr), mult16_32_q15_sse4_1(s8, yar)));
         __m128i s12 = kf_swap_negate_imag(_mm_sub_epi32(mult16_32_q15_sse4_1(s9, yai), mult16_32_q15_sse4_1(s10, ybi)));
         KF_STORE(Fout0, _mm_add_epi32(s0, _mm_add_epi32(s7, s8)));
         KF_STORE(Fout1, _mm_sub_epi32(s5, s6));
         KF_STORE(Fout4, _mm_add_epi32(s5, s6));
         KF_STORE(Fout2, _mm_add_epi32(s11, s12));
         KF_STORE(Fout3, _mm_sub_epi32(s11, s12));
         Fout0+=2;Fout1+=2;Fout2+=2;Fout3+=2;Fout4+=2;
      }
   }
}


#ifndef OVERRIDE_fft_downshift
static void fft_downshift(kiss_fft_cpx *x, int N, int *total, int step) {
   int shift;
   shift = IMIN(step, *total);
   *total -= shift;
   if (shift == 1) {
      int i;
      for (i=0;i<N;i++) {
         x[i].r = SHR32(x[i].r, 1);
         x[i].i = SHR32(x[i].i, 1);
      }
   } else if (shift>0) {
      int i;
      for (i=0;i<N;i++) {
         x[i].r = PSHR32(x[i].r, shift);
         x[i].i = PSHR32(x[i].i, shift);
      }
   }
}
#endif /* OVERRIDE_fft_downshift */

void opus_fft_impl(const kiss_fft_state *st,kiss_fft_cpx *fout, int downshift)
{
    int m2, m;
    int p;
    int L;
    int fstride[MAXFACTORS];
    int i;
    int shift;

    /* st->shift can be -1 */
    shift = st->shift>0 ? st->shift : 0;

    fstride[0] = 1;
    L=0;
    do {
       p = st->factors[2*L];
       m = st->factors[2*L+1];
       fstride[L+1] = fstride[L]*p;
       L++;
    } while(m!=1);
    m = st->factors[2*L-1];
    for (i=L-1;i>=0;i--)
    {
       if (i!=0)
          m2 = st->factors[2*i-1];
       else
          m2 = 1;
       switch (st->factors[2*i])
       {
       case 2:
          fft_downshift(fout, st->nfft, &downshift, 1);
          kf_bfly2(fout, m, fstride[i]);
          break;
       case 4:
          fft_downshift(fout, st->nfft, &downshift, 2);
          kf_bfly4(fout,fstride[i]<<shift,st,m, fstride[i], m2);
          break;
 #ifndef RADIX_TWO_ONLY
       case 3:
          fft_downshift(fout, st->nfft, &downshift, 2);
          kf_bfly3(fout,fstride[i]<<shift,st,m, fstride[i], m2);
          break;
       case 5:
          fft_downshift(fout, st->nfft, &downshift, 3);
          kf_bfly5(fout,fstride[i]<<shift,st,m, fstride[i], m2);
          break;
 #endif
       }
       m = m2;
    }
    fft_downshift(fout, st->nfft, &downshift, downshift);
}

