/* Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2008 Xiph.Org Foundation
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

/* This is a simple MDCT implementation that uses a N/4 complex FFT
   to do most of the work. It should be relatively straightforward to
   plug in pretty much and FFT here.

   This replaces the Vorbis FFT (and uses the exact same API), which
   was a bit too messy and that was ending up duplicating code
   (might as well use the same FFT everywhere).

   The algorithm is similar to (and inspired from) Fabrice Bellard's
   MDCT implementation in FFMPEG, but has differences in signs, ordering
   and scaling in many places.
*/


#include "mdct.h"
#include "kiss_fft.h"
#include "_kiss_fft_guts.h"
#include <math.h>
#include "os_support.h"
#include "mathops.h"
#include "stack_alloc.h"
#include "x86/x86cpu.h"


#ifndef M_PI
#define M_PI 3.141592653
#endif


/* Forward MDCT trashes the input array */
#ifndef OVERRIDE_clt_mdct_forward
void clt_mdct_forward_c(const mdct_lookup *l, kiss_fft_scalar *in, kiss_fft_scalar * OPUS_RESTRICT out,
      const celt_coef *window, int overlap, int shift, int stride, int arch, char *scratch)
{
   int i;
   int N, N2, N4;
   VARDECL(kiss_fft_scalar, f);
   VARDECL(kiss_fft_cpx, f2);
   const kiss_fft_state *st = l->kfft[shift];
   const kiss_twiddle_scalar *trig;
   celt_coef scale;
   /* Allows us to scale with MULT16_32_Q16(), which is faster than
      MULT16_32_Q15() on ARM. */
   int scale_shift = st->scale_shift-1;
   int headroom;
   SAVE_STACK;
   (void)arch;
   scale = st->scale;

   N = l->n;
   trig = l->trig;
   for (i=0;i<shift;i++)
   {
      N >>= 1;
      trig += N;
   }
   N2 = N>>1;
   N4 = N>>2;

   ALLOC(f, N2, kiss_fft_scalar);
   ALLOC(f2, N4, kiss_fft_cpx);

   /* Consider the input to be composed of four blocks: [a, b, c, d] */
   /* Window, shuffle, fold */
   {
      /* Temp pointers to make it really clear to the compiler what we're doing */
      const kiss_fft_scalar * OPUS_RESTRICT xp1 = in+(overlap>>1);
      const kiss_fft_scalar * OPUS_RESTRICT xp2 = in+N2-1+(overlap>>1);
      kiss_fft_scalar * OPUS_RESTRICT yp = f;
      const celt_coef * OPUS_RESTRICT wp1 = window+(overlap>>1);
      const celt_coef * OPUS_RESTRICT wp2 = window+(overlap>>1)-1;
      for(i=0;i<((overlap+3)>>2);i++)
      {
         /* Real part arranged as -d-cR, Imag part arranged as -b+aR*/
         *yp++ = S_MUL(xp1[N2], *wp2) + S_MUL(*xp2, *wp1);
         *yp++ = S_MUL(*xp1, *wp1)    - S_MUL(xp2[-N2], *wp2);
         xp1+=2;
         xp2-=2;
         wp1+=2;
         wp2-=2;
      }
      wp1 = window;
      wp2 = window+overlap-1;
      for(;i<N4-((overlap+3)>>2);i++)
      {
         /* Real part arranged as a-bR, Imag part arranged as -c-dR */
         *yp++ = *xp2;
         *yp++ = *xp1;
         xp1+=2;
         xp2-=2;
      }
      for(;i<N4;i++)
      {
         /* Real part arranged as a-bR, Imag part arranged as -c-dR */
         *yp++ =  -S_MUL(xp1[-N2], *wp1) + S_MUL(*xp2, *wp2);
         *yp++ = S_MUL(*xp1, *wp2)     + S_MUL(xp2[N2], *wp1);
         xp1+=2;
         xp2-=2;
         wp1+=2;
         wp2-=2;
      }
   }
   /* Pre-rotation. maiopus: SSE4.1, four rotations at a time (N4 is 480,
      240, 120 or 60), each stored as one 64-bit pair in bit-reversed order. */
   {
      kiss_fft_scalar * OPUS_RESTRICT yp = f;
      const kiss_twiddle_scalar *t = &trig[0];
      const __m128i vscale = _mm_set1_epi32(scale);
      __m128i vmax = _mm_set1_epi32(1);
      opus_val32 maxval;
      for(i=0;i<N4;i+=4)
      {
         __m128 a = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)yp));
         __m128 b = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)(yp+4)));
         __m128i re = _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2,0,2,0)));
         __m128i im = _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3,1,3,1)));
         __m128i t0 = OP_CVTEPI16_EPI32_M64(&t[i]);
         __m128i t1 = OP_CVTEPI16_EPI32_M64(&t[N4+i]);
         /* For QEXT, it's best to scale before the FFT, but otherwise it's best to scale after.
            For floating-point it doesn't matter. */
         __m128i yr = mult16_32_q16_sse4_1(_mm_sub_epi32(mult16_32_q15_sse4_1(re, t0), mult16_32_q15_sse4_1(im, t1)), vscale);
         __m128i yi = mult16_32_q16_sse4_1(_mm_add_epi32(mult16_32_q15_sse4_1(im, t0), mult16_32_q15_sse4_1(re, t1)), vscale);
         __m128i lo = _mm_unpacklo_epi32(yr, yi);
         __m128i hi = _mm_unpackhi_epi32(yr, yi);
         vmax = _mm_max_epi32(vmax, _mm_max_epi32(_mm_abs_epi32(yr), _mm_abs_epi32(yi)));
         _mm_storel_epi64((__m128i *)(void*)&f2[st->bitrev[i]], lo);
         _mm_storel_epi64((__m128i *)(void*)&f2[st->bitrev[i+1]], _mm_srli_si128(lo, 8));
         _mm_storel_epi64((__m128i *)(void*)&f2[st->bitrev[i+2]], hi);
         _mm_storel_epi64((__m128i *)(void*)&f2[st->bitrev[i+3]], _mm_srli_si128(hi, 8));
         yp += 8;
      }
      vmax = _mm_max_epi32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1,0,3,2)));
      vmax = _mm_max_epi32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(2,3,0,1)));
      maxval = _mm_cvtsi128_si32(vmax);
      headroom = IMAX(0, IMIN(scale_shift, 28-celt_ilog2(maxval)));
   }

   /* N/4 complex FFT, does not downscale anymore */
   opus_fft_impl(st, f2, scale_shift-headroom);

   /* Post-rotate */
   {
      /* Temp pointers to make it really clear to the compiler what we're doing */
      const kiss_fft_cpx * OPUS_RESTRICT fp = f2;
      kiss_fft_scalar * OPUS_RESTRICT yp1 = out;
      kiss_fft_scalar * OPUS_RESTRICT yp2 = out+stride*(N2-1);
      const kiss_twiddle_scalar *t = &trig[0];
      int end = N4;
      i=0;
      /* maiopus: four rotations from the front and four from the back at a
         time: their outputs interleave into one block at each end. */
      if (stride == 1)
      {
         const __m128i hshift = _mm_cvtsi32_si128(headroom);
         const __m128i round = _mm_set1_epi32(EXTEND32(1)<<headroom>>1);
         for(;2*i+7<N4;i+=4)
         {
            const int j = N4-4-i;
            __m128 fa = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)&f2[i]));
            __m128 fb = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)&f2[i+2]));
            __m128 ba = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)&f2[j]));
            __m128 bb = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)&f2[j+2]));
            __m128i fr = _mm_castps_si128(_mm_shuffle_ps(fa, fb, _MM_SHUFFLE(2,0,2,0)));
            __m128i fi = _mm_castps_si128(_mm_shuffle_ps(fa, fb, _MM_SHUFFLE(3,1,3,1)));
            __m128i br = _mm_castps_si128(_mm_shuffle_ps(ba, bb, _MM_SHUFFLE(2,0,2,0)));
            __m128i bi = _mm_castps_si128(_mm_shuffle_ps(ba, bb, _MM_SHUFFLE(3,1,3,1)));
            __m128i ft0 = OP_CVTEPI16_EPI32_M64(&t[i]), ft1 = OP_CVTEPI16_EPI32_M64(&t[N4+i]);
            __m128i bt0 = OP_CVTEPI16_EPI32_M64(&t[j]), bt1 = OP_CVTEPI16_EPI32_M64(&t[N4+j]);
            __m128i fyr = _mm_sra_epi32(_mm_add_epi32(_mm_sub_epi32(mult16_32_q15_sse4_1(fi, ft1), mult16_32_q15_sse4_1(fr, ft0)), round), hshift);
            __m128i fyi = _mm_sra_epi32(_mm_add_epi32(_mm_add_epi32(mult16_32_q15_sse4_1(fr, ft1), mult16_32_q15_sse4_1(fi, ft0)), round), hshift);
            __m128i byr = _mm_sra_epi32(_mm_add_epi32(_mm_sub_epi32(mult16_32_q15_sse4_1(bi, bt1), mult16_32_q15_sse4_1(br, bt0)), round), hshift);
            __m128i byi = _mm_sra_epi32(_mm_add_epi32(_mm_add_epi32(mult16_32_q15_sse4_1(br, bt1), mult16_32_q15_sse4_1(bi, bt0)), round), hshift);
            /* Front i+k writes out[2i+2k] and out[N2-1-2i-2k]; back j+k writes
               out[2j+2k] and out[N2-1-2j-2k], so the odd slots of each block
               take the other end's imaginary parts in reverse. */
            byi = _mm_shuffle_epi32(byi, _MM_SHUFFLE(0,1,2,3));
            fyi = _mm_shuffle_epi32(fyi, _MM_SHUFFLE(0,1,2,3));
            _mm_storeu_si128((__m128i *)(void*)&out[2*i], _mm_unpacklo_epi32(fyr, byi));
            _mm_storeu_si128((__m128i *)(void*)&out[2*i+4], _mm_unpackhi_epi32(fyr, byi));
            _mm_storeu_si128((__m128i *)(void*)&out[2*j], _mm_unpacklo_epi32(byr, fyi));
            _mm_storeu_si128((__m128i *)(void*)&out[2*j+4], _mm_unpackhi_epi32(byr, fyi));
         }
         end = N4-i;
         fp += i;
         yp1 += 2*i;
         yp2 -= 2*i;
      }
      for(;i<end;i++)
      {
         kiss_fft_scalar yr, yi;
         kiss_fft_scalar t0, t1;
         t0 = t[i];
         t1 = t[N4+i];
         yr = PSHR32(S_MUL(fp->i,t1) - S_MUL(fp->r,t0), headroom);
         yi = PSHR32(S_MUL(fp->r,t1) + S_MUL(fp->i,t0), headroom);
         *yp1 = yr;
         *yp2 = yi;
         fp++;
         yp1 += 2*stride;
         yp2 -= 2*stride;
      }
   }
   RESTORE_STACK;
}
#endif /* OVERRIDE_clt_mdct_forward */

#ifndef OVERRIDE_clt_mdct_backward
void clt_mdct_backward_c(const mdct_lookup *l, kiss_fft_scalar *in, kiss_fft_scalar * OPUS_RESTRICT out,
      const celt_coef * OPUS_RESTRICT window, int overlap, int shift, int stride, int arch)
{
   int i;
   int N, N2, N4;
   const kiss_twiddle_scalar *trig;
   int pre_shift, post_shift, fft_shift;
   (void) arch;

   N = l->n;
   trig = l->trig;
   for (i=0;i<shift;i++)
   {
      N >>= 1;
      trig += N;
   }
   N2 = N>>1;
   N4 = N>>2;

   /* maiopus: SSE4.1, four values per step in every pass below: N2 (960,
      480, 240 or 120), N4 and overlap/2 (60) are multiples of four and N4/2
      is even. The lanes' sums and products wrap and truncate exactly as the
      scalar macros. */
   {
      __m128i vmax = _mm_setzero_si128();
      __m128i vsum = _mm_setzero_si128();
      for (i=0;i<N2;i+=4) {
         /* Gathered for short blocks. */
         __m128i v = stride == 1 ? _mm_loadu_si128((__m128i *)(void*)&in[i])
               : _mm_set_epi32(in[(i+3)*stride], in[(i+2)*stride], in[(i+1)*stride], in[i*stride]);
         vmax = _mm_max_epi32(vmax, _mm_abs_epi32(v));
         vsum = _mm_add_epi32(vsum, _mm_abs_epi32(_mm_srai_epi32(v, 11)));
      }
      vmax = _mm_max_epi32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1,0,3,2)));
      vmax = _mm_max_epi32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(2,3,0,1)));
      vsum = _mm_add_epi32(vsum, _mm_shuffle_epi32(vsum, _MM_SHUFFLE(1,0,3,2)));
      vsum = _mm_add_epi32(vsum, _mm_shuffle_epi32(vsum, _MM_SHUFFLE(2,3,0,1)));
      {
         opus_val32 maxval = _mm_cvtsi128_si32(vmax);
         opus_val32 sumval = ADD32_ovflw(N2, _mm_cvtsi128_si32(vsum));
         pre_shift = IMAX(0, 29-celt_zlog2(1+maxval));
         /* Worst-case where all the energy goes to a single sample. */
         post_shift = IMAX(0, 19-celt_ilog2(ABS32(sumval)));
         post_shift = IMIN(post_shift, pre_shift);
         fft_shift = pre_shift - post_shift;
      }
   }
   /* Pre-rotate, each result pair stored with one 64-bit store directly in
      bit-reversed order */
   {
      /* Temp pointers to make it really clear to the compiler what we're doing */
      const kiss_fft_scalar * OPUS_RESTRICT xp1 = in;
      const kiss_fft_scalar * OPUS_RESTRICT xp2 = in+stride*(N2-1);
      kiss_fft_scalar * OPUS_RESTRICT yp = out+(overlap>>1);
      const kiss_twiddle_scalar * OPUS_RESTRICT t = &trig[0];
      const opus_int16 * OPUS_RESTRICT bitrev = l->kfft[shift]->bitrev;
      const __m128i pre = _mm_cvtsi32_si128(pre_shift);
      const int step = 2*stride;
      for (i=0;i<N4;i+=4)
      {
         __m128i x1, x2, t0, t1, yr, yi, lo, hi;
         if (stride == 1)
         {
            __m128 a = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)xp1));
            __m128 b = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)(xp1+4)));
            __m128 c = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)(xp2-7)));
            __m128 d = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)(xp2-3)));
            x1 = _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2,0,2,0)));
            x2 = _mm_castps_si128(_mm_shuffle_ps(d, c, _MM_SHUFFLE(1,3,1,3)));
         } else {
            x1 = _mm_set_epi32(xp1[3*step], xp1[2*step], xp1[step], xp1[0]);
            x2 = _mm_set_epi32(xp2[-3*step], xp2[-2*step], xp2[-step], xp2[0]);
         }
         x1 = _mm_sll_epi32(x1, pre);
         x2 = _mm_sll_epi32(x2, pre);
         t0 = OP_CVTEPI16_EPI32_M64(&t[i]);
         t1 = OP_CVTEPI16_EPI32_M64(&t[N4+i]);
         yr = _mm_add_epi32(mult16_32_q15_sse4_1(x2, t0), mult16_32_q15_sse4_1(x1, t1));
         yi = _mm_sub_epi32(mult16_32_q15_sse4_1(x1, t0), mult16_32_q15_sse4_1(x2, t1));
         /* We swap real and imag because we use an FFT instead of an IFFT. */
         lo = _mm_unpacklo_epi32(yi, yr);
         hi = _mm_unpackhi_epi32(yi, yr);
         _mm_storel_epi64((__m128i *)(void*)&yp[2*bitrev[0]], lo);
         _mm_storel_epi64((__m128i *)(void*)&yp[2*bitrev[1]], _mm_srli_si128(lo, 8));
         _mm_storel_epi64((__m128i *)(void*)&yp[2*bitrev[2]], hi);
         _mm_storel_epi64((__m128i *)(void*)&yp[2*bitrev[3]], _mm_srli_si128(hi, 8));
         bitrev += 4;
         xp1 += 4*step;
         xp2 -= 4*step;
      }
   }

   opus_fft_impl(l->kfft[shift], (kiss_fft_cpx*)(out+(overlap>>1)), fft_shift);

   /* Post-rotate and de-shuffle from both ends of the buffer at once to make
      it in-place: two steps at a time, so four rotations, the front pair and
      the back pair, whose blocks never meet. */
   {
      kiss_fft_scalar * yp0 = out+(overlap>>1);
      kiss_fft_scalar * yp1 = out+(overlap>>1)+N2-2;
      const kiss_twiddle_scalar *t = &trig[0];
      const __m128i post = _mm_cvtsi32_si128(post_shift);
      const __m128i round = _mm_set1_epi32(EXTEND32(1)<<post_shift>>1);
      for(i=0;i<N4>>1;i+=2)
      {
         /* We swap real and imag because we're using an FFT instead of an IFFT. */
         __m128 front = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)yp0));
         __m128 back = _mm_castsi128_ps(_mm_loadu_si128((__m128i *)(void*)(yp1-2)));
         __m128i re = _mm_castps_si128(_mm_shuffle_ps(front, back, _MM_SHUFFLE(1,3,3,1)));
         __m128i im = _mm_castps_si128(_mm_shuffle_ps(front, back, _MM_SHUFFLE(0,2,2,0)));
         __m128i t0 = _mm_set_epi32(t[N4-i-2], t[N4-i-1], t[i+1], t[i]);
         __m128i t1 = _mm_set_epi32(t[N2-i-2], t[N2-i-1], t[N4+i+1], t[N4+i]);
         /* We'd scale up by 2 here, but instead it's done when mixing the windows */
         __m128i yr = _mm_add_epi32(mult16_32_q15_sse4_1(re, t0), mult16_32_q15_sse4_1(im, t1));
         __m128i yi = _mm_sub_epi32(mult16_32_q15_sse4_1(re, t1), mult16_32_q15_sse4_1(im, t0));
         yr = _mm_sra_epi32(_mm_add_epi32(yr, round), post);
         yi = _mm_sra_epi32(_mm_add_epi32(yi, round), post);
         _mm_storeu_si128((__m128i *)(void*)yp0,
               _mm_unpacklo_epi32(yr, _mm_shuffle_epi32(yi, _MM_SHUFFLE(1,0,3,2))));
         _mm_storeu_si128((__m128i *)(void*)(yp1-2),
               _mm_unpacklo_epi32(_mm_shuffle_epi32(yr, _MM_SHUFFLE(0,1,2,3)),
                                  _mm_shuffle_epi32(yi, _MM_SHUFFLE(2,3,0,1))));
         yp0 += 4;
         yp1 -= 4;
      }
   }

   /* Mirror on both sides for TDAC, four pairs at a time from both ends */
   {
      kiss_fft_scalar * OPUS_RESTRICT xp1 = out+overlap-1;
      kiss_fft_scalar * OPUS_RESTRICT yp1 = out;
      const celt_coef * OPUS_RESTRICT wp1 = window;
      const celt_coef * OPUS_RESTRICT wp2 = window+overlap-1;

      for(i = 0; i < overlap/2; i+=4)
      {
         __m128i x2 = _mm_loadu_si128((__m128i *)(void*)yp1);
         __m128i x1 = _mm_shuffle_epi32(_mm_loadu_si128((__m128i *)(void*)(xp1-3)), _MM_SHUFFLE(0,1,2,3));
         __m128i w1 = OP_CVTEPI16_EPI32_M64(wp1);
         __m128i w2 = _mm_shuffle_epi32(OP_CVTEPI16_EPI32_M64(wp2-3), _MM_SHUFFLE(0,1,2,3));
         __m128i y = _mm_sub_epi32(mult16_32_q15_sse4_1(x2, w2), mult16_32_q15_sse4_1(x1, w1));
         __m128i x = _mm_add_epi32(mult16_32_q15_sse4_1(x2, w1), mult16_32_q15_sse4_1(x1, w2));
         _mm_storeu_si128((__m128i *)(void*)yp1, y);
         _mm_storeu_si128((__m128i *)(void*)(xp1-3), _mm_shuffle_epi32(x, _MM_SHUFFLE(0,1,2,3)));
         yp1 += 4;
         xp1 -= 4;
         wp1 += 4;
         wp2 -= 4;
      }
   }
}
#endif /* OVERRIDE_clt_mdct_backward */
