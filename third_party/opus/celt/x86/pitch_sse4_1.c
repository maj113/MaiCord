/* Copyright (c) 2014, Cisco Systems, INC
   Written by XiangMingZhu WeiZhou MinPeng YanWang

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


#include <xmmintrin.h>
#include <emmintrin.h>

#include "macros.h"
#include "celt_lpc.h"
#include "stack_alloc.h"
#include "mathops.h"
#include "pitch.h"

#if defined(OPUS_X86_MAY_HAVE_SSE4_1) && defined(FIXED_POINT)
#include <smmintrin.h>
#include "x86cpu.h"

opus_val32 celt_inner_prod_sse4_1(const opus_val16 *x, const opus_val16 *y,
      int N)
{
    opus_int  i, dataSize16;
    opus_int32 sum;
    __m128i inVec1_76543210, inVec1_FEDCBA98, acc1;
    __m128i inVec2_76543210, inVec2_FEDCBA98, acc2;
    __m128i inVec1_3210, inVec2_3210;

    sum = 0;
    dataSize16 = N & ~15;

    acc1 = _mm_setzero_si128();
    acc2 = _mm_setzero_si128();

    for (i=0;i<dataSize16;i+=16) {
        inVec1_76543210 = _mm_loadu_si128((__m128i *)(void*)(&x[i + 0]));
        inVec2_76543210 = _mm_loadu_si128((__m128i *)(void*)(&y[i + 0]));

        inVec1_FEDCBA98 = _mm_loadu_si128((__m128i *)(void*)(&x[i + 8]));
        inVec2_FEDCBA98 = _mm_loadu_si128((__m128i *)(void*)(&y[i + 8]));

        inVec1_76543210 = _mm_madd_epi16(inVec1_76543210, inVec2_76543210);
        inVec1_FEDCBA98 = _mm_madd_epi16(inVec1_FEDCBA98, inVec2_FEDCBA98);

        acc1 = _mm_add_epi32(acc1, inVec1_76543210);
        acc2 = _mm_add_epi32(acc2, inVec1_FEDCBA98);
    }

    acc1 = _mm_add_epi32(acc1, acc2);

    if (N - i >= 8)
    {
        inVec1_76543210 = _mm_loadu_si128((__m128i *)(void*)(&x[i + 0]));
        inVec2_76543210 = _mm_loadu_si128((__m128i *)(void*)(&y[i + 0]));

        inVec1_76543210 = _mm_madd_epi16(inVec1_76543210, inVec2_76543210);

        acc1 = _mm_add_epi32(acc1, inVec1_76543210);
        i += 8;
    }

    if (N - i >= 4)
    {
        inVec1_3210 = OP_CVTEPI16_EPI32_M64(&x[i + 0]);
        inVec2_3210 = OP_CVTEPI16_EPI32_M64(&y[i + 0]);

        inVec1_3210 = _mm_mullo_epi32(inVec1_3210, inVec2_3210);

        acc1 = _mm_add_epi32(acc1, inVec1_3210);
        i += 4;
    }

    acc1 = _mm_add_epi32(acc1, _mm_unpackhi_epi64(acc1, acc1));
    acc1 = _mm_add_epi32(acc1, _mm_shufflelo_epi16(acc1, 0x0E));

    sum += _mm_cvtsi128_si32(acc1);

    for (;i<N;i++)
    {
        sum = silk_SMLABB(sum, x[i], y[i]);
    }

    return sum;
}

void xcorr_kernel_sse4_1(const opus_val16 * x, const opus_val16 * y, opus_val32 sum[ 4 ], int len)
{
    int j;

    __m128i vecX, vecX0, vecX1, vecX2, vecX3;
    __m128i vecY0, vecY1, vecY2, vecY3;
    __m128i sum0, sum1, sum2, sum3, vecSum;
    __m128i initSum;


    celt_assert(len >= 3);

    sum0 = _mm_setzero_si128();
    sum1 = _mm_setzero_si128();
    sum2 = _mm_setzero_si128();
    sum3 = _mm_setzero_si128();

    for (j=0;j<(len-7);j+=8)
    {
        vecX = _mm_loadu_si128((__m128i *)(void*)(&x[j + 0]));
        vecY0 = _mm_loadu_si128((__m128i *)(void*)(&y[j + 0]));
        vecY1 = _mm_loadu_si128((__m128i *)(void*)(&y[j + 1]));
        vecY2 = _mm_loadu_si128((__m128i *)(void*)(&y[j + 2]));
        vecY3 = _mm_loadu_si128((__m128i *)(void*)(&y[j + 3]));

        sum0 = _mm_add_epi32(sum0, _mm_madd_epi16(vecX, vecY0));
        sum1 = _mm_add_epi32(sum1, _mm_madd_epi16(vecX, vecY1));
        sum2 = _mm_add_epi32(sum2, _mm_madd_epi16(vecX, vecY2));
        sum3 = _mm_add_epi32(sum3, _mm_madd_epi16(vecX, vecY3));
    }

    sum0 = _mm_add_epi32(sum0, _mm_unpackhi_epi64( sum0, sum0));
    sum0 = _mm_add_epi32(sum0, _mm_shufflelo_epi16( sum0, 0x0E));

    sum1 = _mm_add_epi32(sum1, _mm_unpackhi_epi64( sum1, sum1));
    sum1 = _mm_add_epi32(sum1, _mm_shufflelo_epi16( sum1, 0x0E));

    sum2 = _mm_add_epi32(sum2, _mm_unpackhi_epi64( sum2, sum2));
    sum2 = _mm_add_epi32(sum2, _mm_shufflelo_epi16( sum2, 0x0E));

    sum3 = _mm_add_epi32(sum3, _mm_unpackhi_epi64( sum3, sum3));
    sum3 = _mm_add_epi32(sum3, _mm_shufflelo_epi16( sum3, 0x0E));

    vecSum = _mm_unpacklo_epi64(_mm_unpacklo_epi32(sum0, sum1),
          _mm_unpacklo_epi32(sum2, sum3));

    for (;j<(len-3);j+=4)
    {
        vecX = OP_CVTEPI16_EPI32_M64(&x[j + 0]);
        vecX0 = _mm_shuffle_epi32(vecX, 0x00);
        vecX1 = _mm_shuffle_epi32(vecX, 0x55);
        vecX2 = _mm_shuffle_epi32(vecX, 0xaa);
        vecX3 = _mm_shuffle_epi32(vecX, 0xff);

        vecY0 = OP_CVTEPI16_EPI32_M64(&y[j + 0]);
        vecY1 = OP_CVTEPI16_EPI32_M64(&y[j + 1]);
        vecY2 = OP_CVTEPI16_EPI32_M64(&y[j + 2]);
        vecY3 = OP_CVTEPI16_EPI32_M64(&y[j + 3]);

        sum0 = _mm_mullo_epi32(vecX0, vecY0);
        sum1 = _mm_mullo_epi32(vecX1, vecY1);
        sum2 = _mm_mullo_epi32(vecX2, vecY2);
        sum3 = _mm_mullo_epi32(vecX3, vecY3);

        sum0 = _mm_add_epi32(sum0, sum1);
        sum2 = _mm_add_epi32(sum2, sum3);
        vecSum = _mm_add_epi32(vecSum, sum0);
        vecSum = _mm_add_epi32(vecSum, sum2);
    }

    vecX = OP_CVTEPI16_EPI32_M64(&x[len - 4]);
    if (len - j == 3)
    {
        vecX0 = _mm_shuffle_epi32(vecX, 0x55);
        vecX1 = _mm_shuffle_epi32(vecX, 0xaa);
        vecX2 = _mm_shuffle_epi32(vecX, 0xff);

        vecY0 = OP_CVTEPI16_EPI32_M64(&y[j + 0]);
        vecY1 = OP_CVTEPI16_EPI32_M64(&y[j + 1]);
        vecY2 = OP_CVTEPI16_EPI32_M64(&y[j + 2]);

        sum0 = _mm_mullo_epi32(vecX0, vecY0);
        sum1 = _mm_mullo_epi32(vecX1, vecY1);
        sum2 = _mm_mullo_epi32(vecX2, vecY2);

        vecSum = _mm_add_epi32(vecSum, sum0);
        vecSum = _mm_add_epi32(vecSum, sum1);
        vecSum = _mm_add_epi32(vecSum, sum2);
    }
    else if (len - j == 2)
    {
        vecX0 = _mm_shuffle_epi32(vecX, 0xaa);
        vecX1 = _mm_shuffle_epi32(vecX, 0xff);

        vecY0 = OP_CVTEPI16_EPI32_M64(&y[j + 0]);
        vecY1 = OP_CVTEPI16_EPI32_M64(&y[j + 1]);

        sum0 = _mm_mullo_epi32(vecX0, vecY0);
        sum1 = _mm_mullo_epi32(vecX1, vecY1);

        vecSum = _mm_add_epi32(vecSum, sum0);
        vecSum = _mm_add_epi32(vecSum, sum1);
    }
    else if (len - j == 1)
    {
        vecX0 = _mm_shuffle_epi32(vecX, 0xff);

        vecY0 = OP_CVTEPI16_EPI32_M64(&y[j + 0]);

        sum0 = _mm_mullo_epi32(vecX0, vecY0);

        vecSum = _mm_add_epi32(vecSum, sum0);
    }

    initSum = _mm_loadu_si128((__m128i *)(void*)(&sum[0]));
    initSum = _mm_add_epi32(initSum, vecSum);
    _mm_storeu_si128((__m128i *)(void*)sum, initSum);

}

/* maiopus: the constant-gain part of comb_filter() (upstream's
   comb_filter_const_c()) four outputs at a time. In place (y == x)
   the filter is recursive, but every tap is at least T-2 >= 13 samples back,
   so a block of four only reads outputs written before it. */
void comb_filter_const_sse4_1(opus_val32 *y, opus_val32 *x, int T, int N,
      celt_coef g10, celt_coef g11, celt_coef g12)
{
    int i;
    const __m128i vg10 = _mm_set1_epi32(g10);
    const __m128i vg11 = _mm_set1_epi32(g11);
    const __m128i vg12 = _mm_set1_epi32(g12);
    const __m128i one = _mm_set1_epi32(1);
    const __m128i high = _mm_set1_epi32(SIG_SAT);
    const __m128i low = _mm_set1_epi32(-SIG_SAT);
    for (i=0;i<N-3;i+=4)
    {
        __m128i sum = _mm_loadu_si128((__m128i *)(void*)&x[i]);
        __m128i x0 = _mm_loadu_si128((__m128i *)(void*)&x[i-T+2]);
        __m128i x1 = _mm_loadu_si128((__m128i *)(void*)&x[i-T+1]);
        __m128i x2 = _mm_loadu_si128((__m128i *)(void*)&x[i-T]);
        __m128i x3 = _mm_loadu_si128((__m128i *)(void*)&x[i-T-1]);
        __m128i x4 = _mm_loadu_si128((__m128i *)(void*)&x[i-T-2]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(x2, vg10));
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(x1, x3), vg11));
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(x0, x4), vg12));
        /* A bit of bias seems to help here. */
        sum = _mm_sub_epi32(sum, one);
        sum = _mm_max_epi32(_mm_min_epi32(sum, high), low);
        _mm_storeu_si128((__m128i *)(void*)&y[i], sum);
    }
    for (;i<N;i++)
    {
        opus_val32 sum = x[i]
                 + MULT_COEF_32(g10,x[i-T])
                 + MULT_COEF_32(g11,ADD32(x[i-T+1],x[i-T-1]))
                 + MULT_COEF_32(g12,ADD32(x[i-T+2],x[i-T-2]));
        sum = SUB32(sum, 1);
        y[i] = SATURATE(sum, SIG_SAT);
    }
}

/* maiopus: the crossfade at the start of comb_filter(), four outputs at a
   time (see comb_filter_const_sse4_1() for the in-place reads). overlap is
   the mode's 120 or 0, a multiple of four. */
void comb_filter_overlap_sse4_1(opus_val32 *y, opus_val32 *x, int T0, int T1, int overlap,
      celt_coef g00, celt_coef g01, celt_coef g02, celt_coef g10, celt_coef g11, celt_coef g12,
      const celt_coef *window)
{
    int i;
    const __m128i vg00 = _mm_set1_epi32(g00), vg01 = _mm_set1_epi32(g01), vg02 = _mm_set1_epi32(g02);
    const __m128i vg10 = _mm_set1_epi32(g10), vg11 = _mm_set1_epi32(g11), vg12 = _mm_set1_epi32(g12);
    const __m128i coef_one = _mm_set1_epi32(COEF_ONE);
    const __m128i three = _mm_set1_epi32(3);
    const __m128i high = _mm_set1_epi32(SIG_SAT);
    const __m128i low = _mm_set1_epi32(-SIG_SAT);
    for (i=0;i<overlap;i+=4)
    {
        /* MULT_COEF() is MULT16_16_Q15(): the 16-bit operands' product fits 32 bits. */
        __m128i w = OP_CVTEPI16_EPI32_M64(&window[i]);
        __m128i f = _mm_srai_epi32(_mm_mullo_epi32(w, w), 15);
        __m128i nf = _mm_sub_epi32(coef_one, f);
        __m128i sum = _mm_loadu_si128((__m128i *)(void*)&x[i]);
        __m128i a, b;
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T0]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(a, _mm_srai_epi32(_mm_mullo_epi32(nf, vg00), 15)));
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T0+1]);
        b = _mm_loadu_si128((__m128i *)(void*)&x[i-T0-1]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(a, b), _mm_srai_epi32(_mm_mullo_epi32(nf, vg01), 15)));
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T0+2]);
        b = _mm_loadu_si128((__m128i *)(void*)&x[i-T0-2]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(a, b), _mm_srai_epi32(_mm_mullo_epi32(nf, vg02), 15)));
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T1]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(a, _mm_srai_epi32(_mm_mullo_epi32(f, vg10), 15)));
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T1+1]);
        b = _mm_loadu_si128((__m128i *)(void*)&x[i-T1-1]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(a, b), _mm_srai_epi32(_mm_mullo_epi32(f, vg11), 15)));
        a = _mm_loadu_si128((__m128i *)(void*)&x[i-T1+2]);
        b = _mm_loadu_si128((__m128i *)(void*)&x[i-T1-2]);
        sum = _mm_add_epi32(sum, mult16_32_q15_sse4_1(_mm_add_epi32(a, b), _mm_srai_epi32(_mm_mullo_epi32(f, vg12), 15)));
        /* A bit of bias seems to help here. */
        sum = _mm_sub_epi32(sum, three);
        sum = _mm_max_epi32(_mm_min_epi32(sum, high), low);
        _mm_storeu_si128((__m128i *)(void*)&y[i], sum);
    }
}
#endif
