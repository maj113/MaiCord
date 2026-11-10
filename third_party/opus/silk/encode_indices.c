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

/* Encode side-information parameters to payload. The frame is coded independently of the previous one: the first gain,
   the absolute pitch lag and the LTP scaling are always sent in full. */
void silk_encode_indices(
    silk_encoder_state          *psEncC,                        /* I/O  Encoder state                               */
    ec_enc                      *psRangeEnc,                    /* I/O  Compressor data structure                   */
    opus_int                    encode_LBRR                     /* I    Flag indicating LBRR data is being encoded  */
)
{
    opus_int   i, k, typeOffset;
    opus_int16 ec_ix[ MAX_LPC_ORDER ];
    opus_uint8 pred_Q8[ MAX_LPC_ORDER ];
    const SideInfoIndices *psIndices;
    const silk_NLSF_CB_struct *psNLSF_CB = &silk_NLSF_CB_WB;

    if( encode_LBRR ) {
         psIndices = &psEncC->indices_LBRR;
    } else {
         psIndices = &psEncC->indices;
    }

    /*******************************************/
    /* Encode signal type and quantizer offset */
    /*******************************************/
    typeOffset = 2 * psIndices->signalType + psIndices->quantOffsetType;
    celt_assert( typeOffset >= 0 && typeOffset < 6 );
    celt_assert( encode_LBRR == 0 || typeOffset >= 2 );
    if( encode_LBRR || typeOffset >= 2 ) {
        ec_enc_icdf( psRangeEnc, typeOffset - 2, silk_type_offset_VAD_iCDF, 8 );
    } else {
        ec_enc_icdf( psRangeEnc, typeOffset, silk_type_offset_no_VAD_iCDF, 8 );
    }

    /****************/
    /* Encode gains */
    /****************/
    /* first subframe: in two stages, MSB bits followed by 3 LSBs */
    silk_assert( psIndices->GainsIndices[ 0 ] >= 0 && psIndices->GainsIndices[ 0 ] < N_LEVELS_QGAIN );
    ec_enc_icdf( psRangeEnc, silk_RSHIFT( psIndices->GainsIndices[ 0 ], 3 ), silk_gain_iCDF[ psIndices->signalType ], 8 );
    ec_enc_icdf( psRangeEnc, psIndices->GainsIndices[ 0 ] & 7, silk_uniform8_iCDF, 8 );

    /* remaining subframes */
    for( i = 1; i < MAX_NB_SUBFR; i++ ) {
        silk_assert( psIndices->GainsIndices[ i ] >= 0 && psIndices->GainsIndices[ i ] < MAX_DELTA_GAIN_QUANT - MIN_DELTA_GAIN_QUANT + 1 );
        ec_enc_icdf( psRangeEnc, psIndices->GainsIndices[ i ], silk_delta_gain_iCDF, 8 );
    }

    /****************/
    /* Encode NLSFs */
    /****************/
    ec_enc_icdf( psRangeEnc, psIndices->NLSFIndices[ 0 ], &psNLSF_CB->CB1_iCDF[ ( psIndices->signalType >> 1 ) * psNLSF_CB->nVectors ], 8 );
    silk_NLSF_unpack( ec_ix, pred_Q8, psNLSF_CB, psIndices->NLSFIndices[ 0 ] );
    celt_assert( psNLSF_CB->order == MAX_LPC_ORDER );
    for( i = 0; i < psNLSF_CB->order; i++ ) {
        if( psIndices->NLSFIndices[ i+1 ] >= NLSF_QUANT_MAX_AMPLITUDE ) {
            ec_enc_icdf( psRangeEnc, 2 * NLSF_QUANT_MAX_AMPLITUDE, &psNLSF_CB->ec_iCDF[ ec_ix[ i ] ], 8 );
            ec_enc_icdf( psRangeEnc, psIndices->NLSFIndices[ i+1 ] - NLSF_QUANT_MAX_AMPLITUDE, silk_NLSF_EXT_iCDF, 8 );
        } else if( psIndices->NLSFIndices[ i+1 ] <= -NLSF_QUANT_MAX_AMPLITUDE ) {
            ec_enc_icdf( psRangeEnc, 0, &psNLSF_CB->ec_iCDF[ ec_ix[ i ] ], 8 );
            ec_enc_icdf( psRangeEnc, -psIndices->NLSFIndices[ i+1 ] - NLSF_QUANT_MAX_AMPLITUDE, silk_NLSF_EXT_iCDF, 8 );
        } else {
            ec_enc_icdf( psRangeEnc, psIndices->NLSFIndices[ i+1 ] + NLSF_QUANT_MAX_AMPLITUDE, &psNLSF_CB->ec_iCDF[ ec_ix[ i ] ], 8 );
        }
    }

    /* Encode NLSF interpolation factor */
    silk_assert( psIndices->NLSFInterpCoef_Q2 >= 0 && psIndices->NLSFInterpCoef_Q2 < 5 );
    ec_enc_icdf( psRangeEnc, psIndices->NLSFInterpCoef_Q2, silk_NLSF_interpolation_factor_iCDF, 8 );

    if( psIndices->signalType == TYPE_VOICED )
    {
        /*********************/
        /* Encode pitch lags */
        /*********************/
        /* lag index, absolute */
        {
            opus_int32 pitch_high_bits, pitch_low_bits;
            pitch_high_bits = silk_DIV32_16( psIndices->lagIndex, silk_RSHIFT( MAX_FS_KHZ, 1 ) );
            pitch_low_bits = psIndices->lagIndex - silk_SMULBB( pitch_high_bits, silk_RSHIFT( MAX_FS_KHZ, 1 ) );
            silk_assert( pitch_low_bits < MAX_FS_KHZ / 2 );
            silk_assert( pitch_high_bits < 32 );
            ec_enc_icdf( psRangeEnc, pitch_high_bits, silk_pitch_lag_iCDF, 8 );
            ec_enc_icdf( psRangeEnc, pitch_low_bits, silk_uniform8_iCDF, 8 );
        }

        /* Contour index */
        silk_assert( psIndices->contourIndex >= 0 && psIndices->contourIndex < 34 );
        ec_enc_icdf( psRangeEnc, psIndices->contourIndex, silk_pitch_contour_iCDF, 8 );

        /********************/
        /* Encode LTP gains */
        /********************/
        /* PERIndex value */
        silk_assert( psIndices->PERIndex >= 0 && psIndices->PERIndex < 3 );
        ec_enc_icdf( psRangeEnc, psIndices->PERIndex, silk_LTP_per_index_iCDF, 8 );

        /* Codebook Indices */
        for( k = 0; k < MAX_NB_SUBFR; k++ ) {
            silk_assert( psIndices->LTPIndex[ k ] >= 0 && psIndices->LTPIndex[ k ] < ( 8 << psIndices->PERIndex ) );
            ec_enc_icdf( psRangeEnc, psIndices->LTPIndex[ k ], silk_LTP_gain_iCDF_ptrs[ psIndices->PERIndex ], 8 );
        }

        /**********************/
        /* Encode LTP scaling */
        /**********************/
        silk_assert( psIndices->LTP_scaleIndex >= 0 && psIndices->LTP_scaleIndex < 3 );
        ec_enc_icdf( psRangeEnc, psIndices->LTP_scaleIndex, silk_LTPscale_iCDF, 8 );
    }

    /***************/
    /* Encode seed */
    /***************/
    silk_assert( psIndices->Seed >= 0 && psIndices->Seed < 4 );
    ec_enc_icdf( psRangeEnc, psIndices->Seed, silk_uniform4_iCDF, 8 );
}
