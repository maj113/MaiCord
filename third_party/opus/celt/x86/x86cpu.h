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

#if !defined(X86CPU_H)
# define X86CPU_H

/* maiopus: SSE4.1 is required (and selected at compile time), so there are no
   dispatch name macros and no workaround for compilers without
   _mm_loadu_si32(). */
# include <smmintrin.h>

# define OP_CVTEPI8_EPI32_M32(x) \
 (_mm_cvtepi8_epi32(_mm_loadu_si32(x)))

# define OP_CVTEPI16_EPI32_M64(x) \
 (_mm_cvtepi16_epi32(_mm_loadl_epi64((__m128i *)(void*)(x))))

/* maiopus: MULT16_32_Q15() on four lanes, the 32-bit lanes of x times the
   sign-extended 16-bit lanes of g: the low 32 bits of each 48-bit product
   shifted right by 15, exactly as the scalar macro. */
static OPUS_INLINE __m128i mult16_32_q15_sse4_1(__m128i x, __m128i g)
{
   __m128i even = _mm_srli_epi64(_mm_mul_epi32(x, g), 15);
   __m128i odd = _mm_slli_epi64(_mm_mul_epi32(_mm_srli_epi64(x, 32), _mm_srli_epi64(g, 32)), 17);
   return _mm_blend_epi16(even, odd, 0xCC);
}

/* maiopus: MULT16_32_Q16() on four lanes (see mult16_32_q15_sse4_1()). */
static OPUS_INLINE __m128i mult16_32_q16_sse4_1(__m128i x, __m128i g)
{
   __m128i even = _mm_srli_epi64(_mm_mul_epi32(x, g), 16);
   __m128i odd = _mm_slli_epi64(_mm_mul_epi32(_mm_srli_epi64(x, 32), _mm_srli_epi64(g, 32)), 16);
   return _mm_blend_epi16(even, odd, 0xCC);
}

#endif
