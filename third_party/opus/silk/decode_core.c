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


#include "main.h"
#include "stack_alloc.h"

#include <xmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>

/* LPC synthesis of one subframe: x[ i ] = res[ i ] + 16 * pred( x[ i - 1 ] ... x[ i - 16 ] ), with the
   silk_SMLAWB tap sum computed exactly. Taps 0 and 1 (the previous two samples) stay scalar. Taps 2..15 depend only
   on samples at least three behind, so a vector dot product over a sliding history, started when its newest
   sample appears, finishes while the next two samples are being computed; the serial dependency per sample
   shrinks to one tap, an add and the saturations. Each history sample is split into its signed high and
   low halves: (x * a) >> 16 = hi * a + ((lo * a) >> 16) with lo unsigned, and the second term is
   pmulhw( lo, a ) + ( lo < 0 ? a : 0 ) for the low half read as signed. Sums wrap in 32 bits like the scalar code. */
typedef struct {
    __m128i hi_a, hi_b, lo_a, lo_b;   /* lane m of a (b) holds the sample m (8 + m) before the newest one pushed */
} silk_lpc_history;

static OPUS_INLINE void silk_lpc_history_push( silk_lpc_history *g, opus_int32 x )
{
    const __m128i v = _mm_cvtsi32_si128( x );   /* 16-bit lanes: low half, high half */
    g->lo_b = _mm_alignr_epi8( g->lo_b, g->lo_a, 14 );
    g->hi_b = _mm_alignr_epi8( g->hi_b, g->hi_a, 14 );
    g->lo_a = _mm_blend_epi16( _mm_slli_si128( g->lo_a, 2 ), v, 0x01 );
    g->hi_a = _mm_blend_epi16( _mm_slli_si128( g->hi_a, 2 ), _mm_srli_si128( v, 2 ), 0x01 );
}

static OPUS_INLINE opus_int32 silk_lpc_history_dot( const silk_lpc_history *g, __m128i ca, __m128i cb )
{
    const __m128i ones = _mm_set1_epi16( 1 );
    __m128i s, la, lb;
    s  = _mm_add_epi32( _mm_madd_epi16( g->hi_a, ca ), _mm_madd_epi16( g->hi_b, cb ) );
    la = _mm_add_epi16( _mm_mulhi_epi16( g->lo_a, ca ), _mm_and_si128( _mm_srai_epi16( g->lo_a, 15 ), ca ) );
    lb = _mm_add_epi16( _mm_mulhi_epi16( g->lo_b, cb ), _mm_and_si128( _mm_srai_epi16( g->lo_b, 15 ), cb ) );
    s  = _mm_add_epi32( s, _mm_add_epi32( _mm_madd_epi16( la, ones ), _mm_madd_epi16( lb, ones ) ) );
    s  = _mm_add_epi32( s, _mm_shuffle_epi32( s, 0x4E ) );
    s  = _mm_add_epi32( s, _mm_shuffle_epi32( s, 0xB1 ) );
    return _mm_cvtsi128_si32( s );
}

/* x[ 0 .. n - 1 ] are the outputs, x[ -16 .. -1 ] the state; A[ 0 .. 17 ] the coefficients, zero from the order up */
static OPUS_INLINE void silk_LPC_synthesis(
    opus_int32          *x,
    const opus_int32    *res,
    opus_int16          *xq,
    const opus_int16    *A,
    opus_int            order,
    opus_int32          Gain_Q10,
    opus_int            n
)
{
    /* History for the first output: lane m holds x[ -3 - m ], lanes 14 and 15 (older than the state) zero */
    const __m128i lanes = _mm_setr_epi8( 12, 13, 8, 9, 4, 5, 0, 1, 14, 15, 10, 11, 6, 7, 2, 3 );
    const __m128i lane_d = _mm_setr_epi8( 4, 5, 0, 1, -128, -128, -128, -128, 6, 7, 2, 3, -128, -128, -128, -128 );
    const __m128i s_a = _mm_shuffle_epi8( _mm_loadu_si128( (const __m128i *)(const void *)( x - 6 ) ), lanes );
    const __m128i s_b = _mm_shuffle_epi8( _mm_loadu_si128( (const __m128i *)(const void *)( x - 10 ) ), lanes );
    const __m128i s_c = _mm_shuffle_epi8( _mm_loadu_si128( (const __m128i *)(const void *)( x - 14 ) ), lanes );
    const __m128i s_d = _mm_shuffle_epi8( _mm_loadu_si128( (const __m128i *)(const void *)( x - 16 ) ), lane_d );
    const __m128i ca = _mm_loadu_si128( (const __m128i *)(const void *)( A + 2 ) );
    const __m128i cb = _mm_loadu_si128( (const __m128i *)(const void *)( A + 10 ) );
    const opus_int32 a0 = A[ 0 ], a1 = A[ 1 ], bias = silk_RSHIFT( order, 1 );
    silk_lpc_history g;
    opus_int32 R, x1 = x[ -1 ], x2 = x[ -2 ];
    opus_int   i;

    g.lo_a = _mm_unpacklo_epi64( s_a, s_b );
    g.hi_a = _mm_unpackhi_epi64( s_a, s_b );
    g.lo_b = _mm_unpacklo_epi64( s_c, s_d );
    g.hi_b = _mm_unpackhi_epi64( s_c, s_d );
    R = silk_ADD32_ovflw( bias, silk_lpc_history_dot( &g, ca, cb ) );
    for( i = 0; i < n; i++ ) {
        opus_int32 pred, xi, R_next;
        silk_lpc_history_push( &g, x2 );
        R_next = silk_ADD32_ovflw( bias, silk_lpc_history_dot( &g, ca, cb ) );
        pred = silk_ADD32_ovflw( silk_ADD32_ovflw( R, silk_SMULWB( x1, a0 ) ), silk_SMULWB( x2, a1 ) );
        xi = silk_ADD_SAT32( res[ i ], silk_LSHIFT_SAT32( pred, 4 ) );
        x[ i ] = xi;
        xq[ i ] = (opus_int16)silk_SAT16( silk_RSHIFT_ROUND( silk_SMULWW( xi, Gain_Q10 ), 8 ) );
        R = R_next;
        x2 = x1;
        x1 = xi;
    }
}

/**********************************************************/
/* Core decoder. Performs inverse NSQ operation LTP + LPC */
/**********************************************************/
void silk_decode_core(
    silk_decoder_state          *psDec,                         /* I/O  Decoder state                               */
    silk_decoder_control        *psDecCtrl,                     /* I    Decoder control                             */
    opus_int16                  xq[],                           /* O    Decoded speech                              */
    const opus_int16            pulses[ MAX_FRAME_LENGTH ],     /* I    Pulse signal                                */
    int                         arch,                           /* I    Run-time architecture                       */
    char                        *scratch                        /* I    Scratch arena                               */
)
{
    opus_int   i, k, lag = 0, start_idx, sLTP_buf_idx, NLSF_interpolation_flag, signalType;
    opus_int16 *A_Q12, *B_Q14, *pxq, A_Q12_tmp[ MAX_LPC_ORDER + 2 ];
    VARDECL( opus_int16, sLTP );
    VARDECL( opus_int32, sLTP_Q15 );
    opus_int32 LTP_pred_Q13, Gain_Q10, inv_gain_Q31, gain_adj_Q16, rand_seed, offset_Q10;
    const opus_int frame_length = psDec->frame_length, subfr_length = psDec->subfr_length;
    opus_int32 *pred_lag_ptr, *pexc_Q14, *pres_Q14;
    VARDECL( opus_int32, res_Q14 );
    VARDECL( opus_int32, sLPC_Q14 );
    SAVE_STACK;

    silk_assert( psDec->prev_gain_Q16 != 0 );

    ALLOC( sLTP, psDec->ltp_mem_length, opus_int16 );
    ALLOC( sLTP_Q15, psDec->ltp_mem_length + psDec->frame_length, opus_int32 );
    ALLOC( res_Q14, psDec->subfr_length, opus_int32 );
    /* Work around a clang bug (verified with clang 6.0 through clang 20.1.0) that causes the last
       memset to be flagged as an invalid read by valgrind (not caught by asan). */
    ALLOC( sLPC_Q14, psDec->subfr_length + MAX_LPC_ORDER, opus_int32 );

    offset_Q10 = silk_Quantization_Offsets_Q10[ psDec->indices.signalType >> 1 ][ psDec->indices.quantOffsetType ];

    if( psDec->indices.NLSFInterpCoef_Q2 < 1 << 2 ) {
        NLSF_interpolation_flag = 1;
    } else {
        NLSF_interpolation_flag = 0;
    }

    /* Decode excitation */
    rand_seed = psDec->indices.Seed;
    for( i = 0; i < frame_length; i++ ) {
        opus_int32 exc;
        rand_seed = silk_RAND( rand_seed );
        exc = silk_LSHIFT( (opus_int32)pulses[ i ], 14 );
        if( exc > 0 ) {
            exc -= QUANT_LEVEL_ADJUST_Q10 << 4;
        } else
        if( exc < 0 ) {
            exc += QUANT_LEVEL_ADJUST_Q10 << 4;
        }
        exc += offset_Q10 << 4;
        if( rand_seed < 0 ) {
           exc = -exc;
        }
        psDec->exc_Q14[ i ] = exc;

        rand_seed = silk_ADD32_ovflw( rand_seed, pulses[ i ] );
    }

    /* Copy LPC state */
    silk_memcpy( sLPC_Q14, psDec->sLPC_Q14_buf, MAX_LPC_ORDER * sizeof( opus_int32 ) );

    pexc_Q14 = psDec->exc_Q14;
    pxq      = xq;
    sLTP_buf_idx = psDec->ltp_mem_length;
    /* Loop over subframes */
    for( k = 0; k < psDec->nb_subfr; k++ ) {
        pres_Q14 = res_Q14;
        A_Q12 = psDecCtrl->PredCoef_Q12[ k >> 1 ];

        /* Preload LPC coefficients to array on stack. Gives small performance gain */
        silk_memcpy( A_Q12_tmp, A_Q12, psDec->LPC_order * sizeof( opus_int16 ) );
        silk_memset( &A_Q12_tmp[ psDec->LPC_order ], 0, ( MAX_LPC_ORDER + 2 - psDec->LPC_order ) * sizeof( opus_int16 ) );
        B_Q14        = &psDecCtrl->LTPCoef_Q14[ k * LTP_ORDER ];
        signalType   = psDec->indices.signalType;

        Gain_Q10     = silk_RSHIFT( psDecCtrl->Gains_Q16[ k ], 6 );
        inv_gain_Q31 = silk_INVERSE32_varQ( psDecCtrl->Gains_Q16[ k ], 47 );

        /* Calculate gain adjustment factor */
        if( psDecCtrl->Gains_Q16[ k ] != psDec->prev_gain_Q16 ) {
            gain_adj_Q16 =  silk_DIV32_varQ( psDec->prev_gain_Q16, psDecCtrl->Gains_Q16[ k ], 16 );

            /* Scale short term state */
            for( i = 0; i < MAX_LPC_ORDER; i++ ) {
                sLPC_Q14[ i ] = silk_SMULWW( gain_adj_Q16, sLPC_Q14[ i ] );
            }
        } else {
            gain_adj_Q16 = (opus_int32)1 << 16;
        }

        /* Save inv_gain */
        silk_assert( inv_gain_Q31 != 0 );
        psDec->prev_gain_Q16 = psDecCtrl->Gains_Q16[ k ];

        /* Avoid abrupt transition from voiced PLC to unvoiced normal decoding */
        if( psDec->lossCnt && psDec->prevSignalType == TYPE_VOICED &&
            psDec->indices.signalType != TYPE_VOICED && k < MAX_NB_SUBFR/2 ) {

            silk_memset( B_Q14, 0, LTP_ORDER * sizeof( opus_int16 ) );
            B_Q14[ LTP_ORDER/2 ] = SILK_FIX_CONST( 0.25, 14 );

            signalType = TYPE_VOICED;
            psDecCtrl->pitchL[ k ] = psDec->lagPrev;
        }

        if( signalType == TYPE_VOICED ) {
            /* Voiced */
            lag = psDecCtrl->pitchL[ k ];

            /* Re-whitening */
            if( k == 0 || ( k == 2 && NLSF_interpolation_flag ) ) {
                /* Rewhiten with new A coefs */
                start_idx = psDec->ltp_mem_length - lag - psDec->LPC_order - LTP_ORDER / 2;
                celt_packet_check( start_idx > 0 );

                if( k == 2 ) {
                    silk_memcpy( &psDec->outBuf[ psDec->ltp_mem_length ], xq, 2 * psDec->subfr_length * sizeof( opus_int16 ) );
                }

                silk_LPC_analysis_filter( &sLTP[ start_idx ], &psDec->outBuf[ start_idx + k * psDec->subfr_length ],
                    A_Q12, psDec->ltp_mem_length - start_idx, psDec->LPC_order, arch );

                /* After rewhitening the LTP state is unscaled */
                if( k == 0 ) {
                    /* Do LTP downscaling to reduce inter-packet dependency */
                    inv_gain_Q31 = silk_LSHIFT( silk_SMULWB( inv_gain_Q31, psDecCtrl->LTP_scale_Q14 ), 2 );
                }
                for( i = 0; i < lag + LTP_ORDER/2; i++ ) {
                    sLTP_Q15[ sLTP_buf_idx - i - 1 ] = silk_SMULWB( inv_gain_Q31, sLTP[ psDec->ltp_mem_length - i - 1 ] );
                }
            } else {
                /* Update LTP state when Gain changes */
                if( gain_adj_Q16 != (opus_int32)1 << 16 ) {
                    for( i = 0; i < lag + LTP_ORDER/2; i++ ) {
                        sLTP_Q15[ sLTP_buf_idx - i - 1 ] = silk_SMULWW( gain_adj_Q16, sLTP_Q15[ sLTP_buf_idx - i - 1 ] );
                    }
                }
            }
        }

        /* Long-term prediction */
        if( signalType == TYPE_VOICED ) {
            /* Set up pointer */
            pred_lag_ptr = &sLTP_Q15[ sLTP_buf_idx - lag + LTP_ORDER / 2 ];
            for( i = 0; i < subfr_length; i++ ) {
                /* Unrolled loop */
                /* Avoids introducing a bias because silk_SMLAWB() always rounds to -inf */
                LTP_pred_Q13 = 2;
                LTP_pred_Q13 = silk_SMLAWB( LTP_pred_Q13, pred_lag_ptr[  0 ], B_Q14[ 0 ] );
                LTP_pred_Q13 = silk_SMLAWB( LTP_pred_Q13, pred_lag_ptr[ -1 ], B_Q14[ 1 ] );
                LTP_pred_Q13 = silk_SMLAWB( LTP_pred_Q13, pred_lag_ptr[ -2 ], B_Q14[ 2 ] );
                LTP_pred_Q13 = silk_SMLAWB( LTP_pred_Q13, pred_lag_ptr[ -3 ], B_Q14[ 3 ] );
                LTP_pred_Q13 = silk_SMLAWB( LTP_pred_Q13, pred_lag_ptr[ -4 ], B_Q14[ 4 ] );
                pred_lag_ptr++;

                /* Generate LPC excitation */
                pres_Q14[ i ] = silk_ADD_LSHIFT32( pexc_Q14[ i ], LTP_pred_Q13, 1 );

                /* Update states */
                sLTP_Q15[ sLTP_buf_idx ] = silk_LSHIFT( pres_Q14[ i ], 1 );
                sLTP_buf_idx++;
            }
        } else {
            pres_Q14 = pexc_Q14;
        }

        silk_LPC_synthesis( &sLPC_Q14[ MAX_LPC_ORDER ], pres_Q14, pxq, A_Q12_tmp, psDec->LPC_order, Gain_Q10, subfr_length );

        /* Update LPC filter state */
        silk_memcpy( sLPC_Q14, &sLPC_Q14[ subfr_length ], MAX_LPC_ORDER * sizeof( opus_int32 ) );
        pexc_Q14 += subfr_length;
        pxq      += subfr_length;
    }

    /* Save LPC state */
    silk_memcpy( psDec->sLPC_Q14_buf, sLPC_Q14, MAX_LPC_ORDER * sizeof( opus_int32 ) );
    RESTORE_STACK;
}
