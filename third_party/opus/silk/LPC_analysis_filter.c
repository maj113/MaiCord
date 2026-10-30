/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.
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
POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/


#include "SigProc_FIX.h"

#include <xmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>

/*******************************************/
/* LPC analysis filter                     */
/* NB! State is kept internally and the    */
/* filter always starts with zero state    */
/* first d output samples are set to zero  */
/*******************************************/

void silk_LPC_analysis_filter(
    opus_int16                  *out,               /* O    Output signal                                               */
    const opus_int16            *in,                /* I    Input signal                                                */
    const opus_int16            *B,                 /* I    MA prediction coefficients, Q12 [order]                     */
    const opus_int32            len,                /* I    Signal length                                               */
    const opus_int32            d,                  /* I    Filter order                                                */
    int                         arch                /* I    Run-time architecture                                       */
)
{
    opus_int   j;
    int ix;
    opus_int32       out32_Q12, out32;
    const opus_int16 *in_ptr;

    celt_assert( d >= 6 );
    celt_assert( (d & 1) == 0 );
    celt_assert( d <= len );

    (void)arch;
    ix = d;
    if( len - d >= 8 ) {
        /* Eight outputs per block. A madd of the sample pair (in[n-1-j], in[n-2-j]) with the tap pair
           (B[j], B[j+1]) sums two taps exactly; sums wrap in 32 bits like silk_SMLABB_ovflw does.
           The last block overlaps the previous one: every output is a function of the input only. */
        __m128i tap[ SILK_MAX_ORDER_LPC / 2 ];
        for( j = 0; j < d; j += 2 ) {
            tap[ j >> 1 ] = _mm_set1_epi32( (opus_int32)( (opus_uint16)B[ j ] | ( (opus_uint32)(opus_uint16)B[ j + 1 ] << 16 ) ) );
        }
        for( ; ix < len; ix += 8 ) {
            const opus_int   n = ix <= len - 8 ? ix : len - 8;
            const opus_int16 *p = in + n - 1;
            __m128i acc_lo = _mm_setzero_si128(), acc_hi = _mm_setzero_si128(), x, lo, hi;
            for( j = 0; j < d; j += 2 ) {
                const __m128i x0 = _mm_loadu_si128( (const __m128i *)(const void *)( p - j ) );
                const __m128i x1 = _mm_loadu_si128( (const __m128i *)(const void *)( p - j - 1 ) );
                acc_lo = _mm_add_epi32( acc_lo, _mm_madd_epi16( _mm_unpacklo_epi16( x0, x1 ), tap[ j >> 1 ] ) );
                acc_hi = _mm_add_epi32( acc_hi, _mm_madd_epi16( _mm_unpackhi_epi16( x0, x1 ), tap[ j >> 1 ] ) );
            }
            /* (in[n] << 12) - prediction, rounded to Q0 and saturated */
            x  = _mm_loadu_si128( (const __m128i *)(const void *)( in + n ) );
            lo = _mm_sub_epi32( _mm_slli_epi32( _mm_cvtepi16_epi32( x ), 12 ), acc_lo );
            hi = _mm_sub_epi32( _mm_slli_epi32( _mm_cvtepi16_epi32( _mm_srli_si128( x, 8 ) ), 12 ), acc_hi );
            lo = _mm_srai_epi32( _mm_add_epi32( _mm_srai_epi32( lo, 11 ), _mm_set1_epi32( 1 ) ), 1 );
            hi = _mm_srai_epi32( _mm_add_epi32( _mm_srai_epi32( hi, 11 ), _mm_set1_epi32( 1 ) ), 1 );
            _mm_storeu_si128( (__m128i *)(void *)( out + n ), _mm_packs_epi32( lo, hi ) );
        }
        ix = len;
    }
    for( ; ix < len; ix++ ) {
        in_ptr = &in[ ix - 1 ];

        out32_Q12 = silk_SMULBB( in_ptr[  0 ], B[ 0 ] );
        /* Allowing wrap around so that two wraps can cancel each other. The rare
           cases where the result wraps around can only be triggered by invalid streams*/
        out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -1 ], B[ 1 ] );
        out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -2 ], B[ 2 ] );
        out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -3 ], B[ 3 ] );
        out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -4 ], B[ 4 ] );
        out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -5 ], B[ 5 ] );
        for( j = 6; j < d; j += 2 ) {
            out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -j     ], B[ j     ] );
            out32_Q12 = silk_SMLABB_ovflw( out32_Q12, in_ptr[ -j - 1 ], B[ j + 1 ] );
        }

        /* Subtract prediction */
        out32_Q12 = silk_SUB32_ovflw( silk_LSHIFT( (opus_int32)in_ptr[ 1 ], 12 ), out32_Q12 );

        /* Scale to Q0 */
        out32 = silk_RSHIFT_ROUND( out32_Q12, 12 );

        /* Saturate output */
        out[ ix ] = (opus_int16)silk_SAT16( out32 );
    }

    /* Set first d output samples to zero */
    silk_memset( out, 0, d * sizeof( opus_int16 ) );
}
