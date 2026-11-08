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


#include "main_FIX.h"
#include <smmintrin.h>

/* maiopus: SSE4.1. The allpass chain of one sample is serial, but tap i of
   sample n needs only taps i - 1 and i of sample n - 1 and tap i - 1 of sample
   n, so four consecutive samples run as a wavefront: at step d, lane k holds
   tap d - k of sample n + k. Lane 0 takes the previous sample's taps from the
   state, and lane 3 leaves the state for the next four samples.
   silk_SMLAWB( a, b, w ) is a + ( b >> 16 ) * w + ( ( b & 0xFFFF ) * w >> 16 )
   exactly (pmaddwd and pmulhuw; the warping is positive and below 2^15).
   corr[ i ] sums ( t_i * t_0 ) >> ( 2 * QS - QC ) over the samples, with
   t_0 = x << QS, that is ( t_i * x ) >> ( QS - QC ); the steps keep the exact
   64-bit sum of the products t_i * x and the sum of their low QS - QC bits,
   whose difference is the sum of the floored terms times 2^( QS - QC ).
   Taps outside 0..order land in sums that are never read. */
static OPUS_INLINE __m128i silk_warped_step(
    __m128i                         t1,                 /* I    Step d - 1                                  */
    __m128i                         t2,                 /* I    Step d - 2                                  */
    const opus_int32                *state,             /* I    Taps d - 1 and d of the sample before lane 0 */
    __m128i                         w_hi,               /* I    Warping in the high 16 bits of each lane    */
    __m128i                         w_lo                /* I    Warping in the low 16 bits of each lane     */
)
{
    /* Taps d - k and d - k - 1 of sample n + k - 1 */
    const __m128i a    = _mm_alignr_epi8( t1, _mm_set1_epi32( state[ 1 ] ), 12 );
    const __m128i b    = _mm_alignr_epi8( t2, _mm_set1_epi32( state[ 0 ] ), 12 );
    const __m128i diff = _mm_sub_epi32( a, t1 );
    return _mm_add_epi32( b, _mm_add_epi32( _mm_madd_epi16( diff, w_hi ), _mm_mulhi_epu16( diff, w_lo ) ) );
}

static OPUS_INLINE void silk_warped_accumulate(
    __m128i                         *sums,              /* I/O  Even and odd lane products, low bits        */
    __m128i                         t,                  /* I    Taps                                        */
    __m128i                         x,                  /* I    Samples                                     */
    __m128i                         x_odd,              /* I    Samples of lanes 1 and 3 in lanes 0 and 2   */
    __m128i                         low_mask            /* I    The bits the shift drops                    */
)
{
    sums[ 0 ] = _mm_add_epi64( sums[ 0 ], _mm_mul_epi32( t, x ) );
    sums[ 1 ] = _mm_add_epi64( sums[ 1 ], _mm_mul_epi32( _mm_srli_epi64( t, 32 ), x_odd ) );
    sums[ 2 ] = _mm_add_epi32( sums[ 2 ], _mm_and_si128( _mm_mullo_epi16( t, x ), low_mask ) );
}

/* Autocorrelations for a warped frequency axis */
void silk_warped_autocorrelation_FIX(
          opus_int32                *corr,                                  /* O    Result [order + 1]                                                          */
          opus_int                  *scale,                                 /* O    Scaling of the correlation vector                                           */
    const opus_int16                *input,                                 /* I    Input data to correlate                                                     */
    const opus_int                  warping_Q16,                            /* I    Warping coefficient                                                         */
    const opus_int                  length,                                 /* I    Length of input                                                             */
    const opus_int                  order                                   /* I    Correlation order (even)                                                    */
)
{
    opus_int   n, i, d, lsh;
    opus_int32 tmp1_QS, tmp2_QS;
    /* Three entries past the order: lane 0 reads them while the other lanes finish */
    opus_int32 state_QS[ MAX_SHAPE_LPC_ORDER + 4 ] = { 0 };
    opus_int64 corr_QC[  MAX_SHAPE_LPC_ORDER + 1 ] = { 0 };
    __m128i    sums[ MAX_SHAPE_LPC_ORDER + 4 ][ 3 ];
    const __m128i w_hi = _mm_set1_epi32( (opus_int32)( (opus_uint32)warping_Q16 << 16 ) );
    const __m128i w_lo = _mm_set1_epi32( warping_Q16 );
    const __m128i low_mask = _mm_set1_epi32( ( 1 << ( QS - QC ) ) - 1 );
    const opus_int steps = order + 4;

    /* Order must be even */
    celt_assert( ( order & 1 ) == 0 );
    silk_assert( 2 * QS - QC >= 0 );
    celt_assert( warping_Q16 > 0 && warping_Q16 <= 32767 );

    for( d = 0; d < steps; d++ ) {
        sums[ d ][ 0 ] = sums[ d ][ 1 ] = sums[ d ][ 2 ] = _mm_setzero_si128();
    }
    for( n = 0; n + 4 <= length; n += 4 ) {
        const __m128i x     = _mm_cvtepi16_epi32( _mm_loadl_epi64( (const __m128i *)(const void *)&input[ n ] ) );
        const __m128i x_odd = _mm_srli_epi64( x, 32 );
        const __m128i t0    = _mm_slli_epi32( x, QS );
        __m128i t, t1, t2;
        /* Steps 0 to 3 start the samples: lane d takes its input at step d */
        t1 = t0;
        silk_warped_accumulate( sums[ 0 ], t1, x, x_odd, low_mask );
        t = _mm_blend_epi16( silk_warped_step( t1, t1, &state_QS[ 0 ], w_hi, w_lo ), t0, 0xFC );
        t2 = t1;
        t1 = t;
        silk_warped_accumulate( sums[ 1 ], t1, x, x_odd, low_mask );
        t = _mm_blend_epi16( silk_warped_step( t1, t2, &state_QS[ 1 ], w_hi, w_lo ), t0, 0xF0 );
        t2 = t1;
        t1 = t;
        silk_warped_accumulate( sums[ 2 ], t1, x, x_odd, low_mask );
        t = _mm_blend_epi16( silk_warped_step( t1, t2, &state_QS[ 2 ], w_hi, w_lo ), t0, 0xC0 );
        t2 = t1;
        t1 = t;
        silk_warped_accumulate( sums[ 3 ], t1, x, x_odd, low_mask );
        state_QS[ 0 ] = _mm_cvtsi128_si32( _mm_shuffle_epi32( t1, 0xFF ) );
        for( d = 4; d < steps; d++ ) {
            t = silk_warped_step( t1, t2, &state_QS[ d - 1 ], w_hi, w_lo );
            t2 = t1;
            t1 = t;
            silk_warped_accumulate( sums[ d ], t1, x, x_odd, low_mask );
            /* Lane 3: tap d - 3 of the last sample of the four */
            state_QS[ d - 3 ] = _mm_cvtsi128_si32( _mm_shuffle_epi32( t1, 0xFF ) );
        }
    }
    {
        opus_int64 products[ MAX_SHAPE_LPC_ORDER + 4 ][ 4 ];
        opus_int32 low[ MAX_SHAPE_LPC_ORDER + 4 ][ 4 ];
        for( d = 0; d < steps; d++ ) {
            _mm_storeu_si128( (__m128i *)(void *)&products[ d ][ 0 ], sums[ d ][ 0 ] );   /* lanes 0, 2 */
            _mm_storeu_si128( (__m128i *)(void *)&products[ d ][ 2 ], sums[ d ][ 1 ] );   /* lanes 1, 3 */
            _mm_storeu_si128( (__m128i *)(void *)&low[ d ][ 0 ], sums[ d ][ 2 ] );
        }
        for( i = 0; i <= order; i++ ) {
            /* Lane k holds tap i at step i + k */
            const opus_int64 p = products[ i ][ 0 ] + products[ i + 1 ][ 2 ] + products[ i + 2 ][ 1 ] + products[ i + 3 ][ 3 ];
            const opus_int32 l = low[ i ][ 0 ] + low[ i + 1 ][ 1 ] + low[ i + 2 ][ 2 ] + low[ i + 3 ][ 3 ];
            corr_QC[ i ] = silk_RSHIFT64( p - l, QS - QC );
        }
    }

    /* Remaining samples, one at a time */
    for( ; n < length; n++ ) {
        tmp1_QS = silk_LSHIFT32( (opus_int32)input[ n ], QS );
        /* Loop over allpass sections */
        for( i = 0; i < order; i += 2 ) {
            /* Output of allpass section */
            tmp2_QS = silk_SMLAWB( state_QS[ i ], state_QS[ i + 1 ] - tmp1_QS, warping_Q16 );
            state_QS[ i ]  = tmp1_QS;
            corr_QC[  i ] += silk_RSHIFT64( silk_SMULL( tmp1_QS, state_QS[ 0 ] ), 2 * QS - QC );
            /* Output of allpass section */
            tmp1_QS = silk_SMLAWB( state_QS[ i + 1 ], state_QS[ i + 2 ] - tmp2_QS, warping_Q16 );
            state_QS[ i + 1 ]  = tmp2_QS;
            corr_QC[  i + 1 ] += silk_RSHIFT64( silk_SMULL( tmp2_QS, state_QS[ 0 ] ), 2 * QS - QC );
        }
        state_QS[ order ] = tmp1_QS;
        corr_QC[  order ] += silk_RSHIFT64( silk_SMULL( tmp1_QS, state_QS[ 0 ] ), 2 * QS - QC );
    }

    lsh = silk_CLZ64( corr_QC[ 0 ] ) - 35;
    lsh = silk_LIMIT( lsh, -12 - QC, 30 - QC );
    *scale = -( QC + lsh );
    silk_assert( *scale >= -30 && *scale <= 12 );
    if( lsh >= 0 ) {
        for( i = 0; i < order + 1; i++ ) {
            corr[ i ] = (opus_int32)silk_CHECK_FIT32( silk_LSHIFT64( corr_QC[ i ], lsh ) );
        }
    } else {
        for( i = 0; i < order + 1; i++ ) {
            corr[ i ] = (opus_int32)silk_CHECK_FIT32( silk_RSHIFT64( corr_QC[ i ], -lsh ) );
        }
    }
    silk_assert( corr_QC[ 0 ] >= 0 ); /* If breaking, decrease QC*/
}
