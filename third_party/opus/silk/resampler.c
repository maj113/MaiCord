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


/*
 * maiopus: the API rate is 48 kHz on both sides, so only two of the resampling
 * methods remain:
 *   48 kHz -> 8, 12 or 16 kHz (encoder input): AR2 filter followed by FIR
 *     decimation by 6, 4 or 3;
 *   8, 12 or 16 kHz -> 48 kHz (decoder output, and the encoder's conversion
 *     of its buffered input): allpass-based 2x upsampling followed by FIR
 *     interpolation.
 */

#include "resampler_private.h"

/* Delay compensation values to equalize total delay for different modes */
static const opus_int8 delay_enc[ 3 ] = { 18, 10, 12 };    /* 48 kHz to 8, 12, 16 kHz */
static const opus_int8 delay_dec[ 3 ] = {  0,  4,  7 };    /* 8, 12, 16 kHz to 48 kHz */

/* Simple way to make [8000, 12000, 16000] to [0, 1, 2] */
#define rateID(R) ( ( (R) >> 12 ) - 1 )

#define USE_silk_resampler_private_IIR_FIR          (2)
#define USE_silk_resampler_private_down_FIR         (3)

/* Initialize/reset the resampler state for a given pair of input/output sampling rates */
opus_int silk_resampler_init(
    silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
    opus_int32                  Fs_Hz_in,           /* I    Input sampling rate (Hz)                                    */
    opus_int32                  Fs_Hz_out,          /* I    Output sampling rate (Hz)                                   */
    opus_int                    forEnc              /* I    If 1: encoder; if 0: decoder                                */
)
{
    opus_int up2x;

    /* Clear state */
    silk_memset( S, 0, sizeof( silk_resampler_state_struct ) );

    /* Input checking */
    if( forEnc ) {
        if( Fs_Hz_in != 48000 || ( Fs_Hz_out != 8000 && Fs_Hz_out != 12000 && Fs_Hz_out != 16000 ) ) {
            celt_assert( 0 );
            return -1;
        }
        S->inputDelay = delay_enc[ rateID( Fs_Hz_out ) ];
    } else {
        if( ( Fs_Hz_in != 8000 && Fs_Hz_in != 12000 && Fs_Hz_in != 16000 ) || Fs_Hz_out != 48000 ) {
            celt_assert( 0 );
            return -1;
        }
        S->inputDelay = delay_dec[ rateID( Fs_Hz_in ) ];
    }

    S->Fs_in_kHz  = silk_DIV32_16( Fs_Hz_in,  1000 );
    S->Fs_out_kHz = silk_DIV32_16( Fs_Hz_out, 1000 );

    /* Number of samples processed per batch */
    S->batchSize = S->Fs_in_kHz * RESAMPLER_MAX_BATCH_SIZE_MS;

    if( forEnc ) {
        /* Downsample: Fs_out : Fs_in = 1 : 6, 1 : 4 or 1 : 3 */
        S->resampler_function = USE_silk_resampler_private_down_FIR;
        if( Fs_Hz_out == 16000 ) {
            S->Coefs = silk_Resampler_1_3_COEFS;
        } else if( Fs_Hz_out == 12000 ) {
            S->Coefs = silk_Resampler_1_4_COEFS;
        } else {
            S->Coefs = silk_Resampler_1_6_COEFS;
        }
        up2x = 0;
    } else {
        /* Upsample */
        S->resampler_function = USE_silk_resampler_private_IIR_FIR;
        up2x = 1;
    }

    /* Ratio of input/output samples */
    S->invRatio_Q16 = silk_LSHIFT32( silk_DIV32( silk_LSHIFT32( Fs_Hz_in, 14 + up2x ), Fs_Hz_out ), 2 );
    /* Make sure the ratio is rounded up */
    while( silk_SMULWW( S->invRatio_Q16, Fs_Hz_out ) < silk_LSHIFT32( Fs_Hz_in, up2x ) ) {
        S->invRatio_Q16++;
    }

    return 0;
}

/* Resampler: convert from one sampling rate to another */
/* Input and output sampling rate are at most 48000 Hz  */
opus_int silk_resampler(
    silk_resampler_state_struct *S,                 /* I/O  Resampler state                                             */
    opus_int16                  out[],              /* O    Output signal                                               */
    const opus_int16            in[],               /* I    Input signal                                                */
    opus_int32                  inLen,              /* I    Number of input samples                                     */
    char                        *scratch            /* I    Scratch arena                                               */
)
{
    opus_int nSamples;

    /* Need at least 1 ms of input data */
    celt_packet_check( inLen >= S->Fs_in_kHz );
    /* Delay can't exceed the 1 ms of buffering */
    celt_assert( S->inputDelay <= S->Fs_in_kHz );

    nSamples = S->Fs_in_kHz - S->inputDelay;

    /* Copy to delay buffer */
    silk_memcpy( &S->delayBuf[ S->inputDelay ], in, nSamples * sizeof( opus_int16 ) );

    switch( S->resampler_function ) {
        case USE_silk_resampler_private_IIR_FIR:
            silk_resampler_private_IIR_FIR( S, out, S->delayBuf, S->Fs_in_kHz, scratch );
            silk_resampler_private_IIR_FIR( S, &out[ S->Fs_out_kHz ], &in[ nSamples ], inLen - S->Fs_in_kHz, scratch );
            break;
        case USE_silk_resampler_private_down_FIR:
            silk_resampler_private_down_FIR( S, out, S->delayBuf, S->Fs_in_kHz, scratch );
            silk_resampler_private_down_FIR( S, &out[ S->Fs_out_kHz ], &in[ nSamples ], inLen - S->Fs_in_kHz, scratch );
            break;
        default:
            /* A cleared, never initialized state copies, as upstream's copy
               resampler (equal rates) does. */
            silk_memcpy( out, S->delayBuf, S->Fs_in_kHz * sizeof( opus_int16 ) );
            silk_memcpy( &out[ S->Fs_out_kHz ], &in[ nSamples ], ( inLen - S->Fs_in_kHz ) * sizeof( opus_int16 ) );
    }

    /* Copy to delay buffer */
    silk_memcpy( S->delayBuf, &in[ inLen - S->inputDelay ], S->inputDelay * sizeof( opus_int16 ) );

    return 0;
}
