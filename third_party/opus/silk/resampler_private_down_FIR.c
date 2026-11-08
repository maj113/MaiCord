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
#include "resampler_private.h"
#include "stack_alloc.h"

/* maiopus: the encoder decimates 48 kHz by 3, 4 or 6, always with the
   symmetric RESAMPLER_DOWN_ORDER_FIR2 filter and a single fraction. */
static OPUS_INLINE opus_int16 *silk_resampler_private_down_FIR_INTERPOL(
    opus_int16          *out,
    opus_int32          *buf,
    const opus_int16    *FIR_Coefs,
    opus_int32          max_index_Q16,
    opus_int32          index_increment_Q16
)
{
    opus_int32 index_Q16, res_Q6;
    opus_int32 *buf_ptr;

    for( index_Q16 = 0; index_Q16 < max_index_Q16; index_Q16 += index_increment_Q16 ) {
        /* Integer part gives pointer to buffered input */
        buf_ptr = buf + silk_RSHIFT( index_Q16, 16 );

        /* Inner product */
        res_Q6 = silk_SMULWB(         silk_ADD32( buf_ptr[  0 ], buf_ptr[ 35 ] ), FIR_Coefs[  0 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  1 ], buf_ptr[ 34 ] ), FIR_Coefs[  1 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  2 ], buf_ptr[ 33 ] ), FIR_Coefs[  2 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  3 ], buf_ptr[ 32 ] ), FIR_Coefs[  3 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  4 ], buf_ptr[ 31 ] ), FIR_Coefs[  4 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  5 ], buf_ptr[ 30 ] ), FIR_Coefs[  5 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  6 ], buf_ptr[ 29 ] ), FIR_Coefs[  6 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  7 ], buf_ptr[ 28 ] ), FIR_Coefs[  7 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  8 ], buf_ptr[ 27 ] ), FIR_Coefs[  8 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[  9 ], buf_ptr[ 26 ] ), FIR_Coefs[  9 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 10 ], buf_ptr[ 25 ] ), FIR_Coefs[ 10 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 11 ], buf_ptr[ 24 ] ), FIR_Coefs[ 11 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 12 ], buf_ptr[ 23 ] ), FIR_Coefs[ 12 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 13 ], buf_ptr[ 22 ] ), FIR_Coefs[ 13 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 14 ], buf_ptr[ 21 ] ), FIR_Coefs[ 14 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 15 ], buf_ptr[ 20 ] ), FIR_Coefs[ 15 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 16 ], buf_ptr[ 19 ] ), FIR_Coefs[ 16 ] );
        res_Q6 = silk_SMLAWB( res_Q6, silk_ADD32( buf_ptr[ 17 ], buf_ptr[ 18 ] ), FIR_Coefs[ 17 ] );

        /* Scale down, saturate and store in output array */
        *out++ = (opus_int16)silk_SAT16( silk_RSHIFT_ROUND( res_Q6, 6 ) );
    }
    return out;
}

/* Resample with a 2nd order AR filter followed by FIR interpolation */
void silk_resampler_private_down_FIR(
    void                            *SS,            /* I/O  Resampler state             */
    opus_int16                      out[],          /* O    Output signal               */
    const opus_int16                in[],           /* I    Input signal                */
    opus_int32                      inLen,          /* I    Number of input samples     */
    char                            *scratch        /* I    Scratch arena               */
)
{
    silk_resampler_state_struct *S = (silk_resampler_state_struct *)SS;
    opus_int32 nSamplesIn;
    opus_int32 max_index_Q16, index_increment_Q16;
    VARDECL( opus_int32, buf );
    const opus_int16 *FIR_Coefs;
    SAVE_STACK;

    ALLOC( buf, S->batchSize + RESAMPLER_DOWN_ORDER_FIR2, opus_int32 );

    /* Copy buffered samples to start of buffer */
    silk_memcpy( buf, S->sFIR.i32, RESAMPLER_DOWN_ORDER_FIR2 * sizeof( opus_int32 ) );

    FIR_Coefs = &S->Coefs[ 2 ];

    /* Iterate over blocks of frameSizeIn input samples */
    index_increment_Q16 = S->invRatio_Q16;
    while( 1 ) {
        nSamplesIn = silk_min( inLen, S->batchSize );

        /* Second-order AR filter (output in Q8) */
        silk_resampler_private_AR2( S->sIIR, &buf[ RESAMPLER_DOWN_ORDER_FIR2 ], in, S->Coefs, nSamplesIn );

        max_index_Q16 = silk_LSHIFT32( nSamplesIn, 16 );

        /* Interpolate filtered signal */
        out = silk_resampler_private_down_FIR_INTERPOL( out, buf, FIR_Coefs, max_index_Q16, index_increment_Q16 );

        in += nSamplesIn;
        inLen -= nSamplesIn;

        if( inLen > 1 ) {
            /* More iterations to do; copy last part of filtered signal to beginning of buffer */
            silk_memcpy( buf, &buf[ nSamplesIn ], RESAMPLER_DOWN_ORDER_FIR2 * sizeof( opus_int32 ) );
        } else {
            break;
        }
    }

    /* Copy last part of filtered signal to the state for the next call */
    silk_memcpy( S->sFIR.i32, &buf[ nSamplesIn ], RESAMPLER_DOWN_ORDER_FIR2 * sizeof( opus_int32 ) );
    RESTORE_STACK;
}
