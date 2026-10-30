/* Copyright (c) 2013 Jean-Marc Valin and John Ridges
   Copyright (c) 2014, Cisco Systems, INC MingXiang WeiZhou MinPeng YanWang*/
/**
   @file pitch_sse.h
   @brief Pitch analysis
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

#ifndef PITCH_SSE_H
#define PITCH_SSE_H


/* maiopus: SSE4.1 is required, so these are the only implementations. */
void xcorr_kernel_sse4_1(
                    const opus_int16 *x,
                    const opus_int16 *y,
                    opus_val32       sum[4],
                    int              len);
#define xcorr_kernel(x, y, sum, len, arch) \
    ((void)arch, xcorr_kernel_sse4_1(x, y, sum, len))

opus_val32 celt_inner_prod_sse4_1(
    const opus_int16 *x,
    const opus_int16 *y,
    int               N);
#define celt_inner_prod(x, y, N, arch) \
    ((void)arch, celt_inner_prod_sse4_1(x, y, N))

void comb_filter_const_sse4_1(opus_val32 *y, opus_val32 *x, int T, int N,
      celt_coef g10, celt_coef g11, celt_coef g12);
void comb_filter_overlap_sse4_1(opus_val32 *y, opus_val32 *x, int T0, int T1, int overlap,
      celt_coef g00, celt_coef g01, celt_coef g02, celt_coef g10, celt_coef g11, celt_coef g12,
      const celt_coef *window);
#define comb_filter_const(y, x, T, N, g10, g11, g12, arch) \
    ((void)arch, comb_filter_const_sse4_1(y, x, T, N, g10, g11, g12))


#endif
