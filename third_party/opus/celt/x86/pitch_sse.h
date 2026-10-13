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


#if defined(OPUS_X86_MAY_HAVE_SSE4_1) && defined(FIXED_POINT)
void xcorr_kernel_sse4_1(
                    const opus_int16 *x,
                    const opus_int16 *y,
                    opus_val32       sum[4],
                    int              len);
#endif


#if defined(OPUS_X86_PRESUME_SSE4_1) && defined(FIXED_POINT)
#define OVERRIDE_XCORR_KERNEL
#define xcorr_kernel(x, y, sum, len, arch) \
    ((void)arch, xcorr_kernel_sse4_1(x, y, sum, len))

#endif

#if defined(OPUS_X86_MAY_HAVE_SSE4_1) && defined(FIXED_POINT)
opus_val32 celt_inner_prod_sse4_1(
    const opus_int16 *x,
    const opus_int16 *y,
    int               N);
#endif

#if defined(OPUS_X86_MAY_HAVE_SSE2) && defined(FIXED_POINT)
opus_val32 celt_inner_prod_sse2(
    const opus_int16 *x,
    const opus_int16 *y,
    int               N);
#endif


#if defined(OPUS_X86_PRESUME_SSE4_1) && defined(FIXED_POINT)
#define OVERRIDE_CELT_INNER_PROD
#define celt_inner_prod(x, y, N, arch) \
    ((void)arch, celt_inner_prod_sse4_1(x, y, N))

#elif defined(OPUS_X86_PRESUME_SSE2) && defined(FIXED_POINT) && !defined(OPUS_X86_MAY_HAVE_SSE4_1)
#define OVERRIDE_CELT_INNER_PROD
#define celt_inner_prod(x, y, N, arch) \
    ((void)arch, celt_inner_prod_sse2(x, y, N))

#endif


#endif
