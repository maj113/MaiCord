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

#ifndef KISS_FFT_H
#define KISS_FFT_H

#include <stdlib.h>
#include <math.h>
#include "arch.h"
#include "cpu_support.h"

#ifdef USE_SIMD
# include <xmmintrin.h>
# define kiss_fft_scalar __m128
#define KISS_FFT_MALLOC(nbytes) memalign(16,nbytes)
#else
#define KISS_FFT_MALLOC opus_alloc
#endif

#include "arch.h"

#  define kiss_fft_scalar opus_int32
#   define COEF_SHIFT 16

#  define kiss_twiddle_scalar celt_coef

/* Some 32-bit CPUs would load/store a kiss_twiddle_cpx with a single memory
 * access, and could benefit from additional alignment.
 */
#  define KISS_TWIDDLE_CPX_ALIGNMENT (sizeof(opus_int32))


#if defined(__GNUC__) && defined(KISS_TWIDDLE_CPX_ALIGNMENT)
#define KISS_TWIDDLE_CPX_ALIGNED __attribute__((aligned(KISS_TWIDDLE_CPX_ALIGNMENT)))
#else
#define KISS_TWIDDLE_CPX_ALIGNED
#endif

typedef struct {
    kiss_fft_scalar r;
    kiss_fft_scalar i;
}kiss_fft_cpx;

typedef struct {
   kiss_twiddle_scalar r;
   kiss_twiddle_scalar i;
} KISS_TWIDDLE_CPX_ALIGNED kiss_twiddle_cpx;

#define MAXFACTORS 8
/* e.g. an fft of length 128 has 4 factors
 as far as kissfft is concerned
 4*4*4*2
 */

typedef struct arch_fft_state{
   int is_supported;
   void *priv;
} arch_fft_state;

typedef struct kiss_fft_state{
    int nfft;
    celt_coef scale;
    int scale_shift;
    int shift;
    opus_int16 factors[2*MAXFACTORS];
    const opus_int16 *bitrev;
    const kiss_twiddle_cpx *twiddles;
    arch_fft_state *arch_fft;
} kiss_fft_state;


void opus_fft_impl(const kiss_fft_state *st,kiss_fft_cpx *fout, int downshift);

#endif
