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
#include "tuning_parameters.h"
#include "pitch_est_defines.h"

/* Encoding settings for each complexity range: 1, 2, 3, 4-5, 6-7 and 8-10 */
typedef struct {
    opus_int32 pitchEstimationThreshold_Q16;
    opus_int8  pitchEstimationComplexity;
    opus_int8  pitchEstimationLPCOrder;
    opus_int8  shapingLPCOrder;
    opus_int8  la_shape_ms;
    opus_int8  nStatesDelayedDecision;
    opus_int8  useInterpolatedNLSFs;
    opus_int8  NLSF_MSVQ_Survivors;
    opus_int8  warped;
} silk_complexity_setting;

static const silk_complexity_setting silk_complexity_table[ 6 ] = {
    { SILK_FIX_CONST( 0.76, 16 ), SILK_PE_MID_COMPLEX,  8, 14, 5, 1,                  0,  3, 0 },
    { SILK_FIX_CONST( 0.8,  16 ), SILK_PE_MIN_COMPLEX,  6, 12, 3, 2,                  0,  2, 0 },
    { SILK_FIX_CONST( 0.76, 16 ), SILK_PE_MID_COMPLEX,  8, 14, 5, 2,                  0,  4, 0 },
    { SILK_FIX_CONST( 0.74, 16 ), SILK_PE_MID_COMPLEX, 10, 16, 5, 2,                  1,  6, 1 },
    { SILK_FIX_CONST( 0.72, 16 ), SILK_PE_MID_COMPLEX, 12, 20, 5, 3,                  1,  8, 1 },
    { SILK_FIX_CONST( 0.7,  16 ), SILK_PE_MAX_COMPLEX, 16, 24, 5, MAX_DEL_DEC_STATES, 1, 16, 1 }
};
/* Row of silk_complexity_table for each complexity setting 1-10 (entry 0 is never used) */
static const opus_uint8 silk_complexity_row[ 11 ] = { 0, 0, 1, 2, 3, 3, 4, 4, 5, 5, 5 };

/* Control encoder: complexity, packet loss rate measured by farend and LBRR usage, set before every packet */
void silk_control_encoder(
    silk_encoder_state              *psEncC,                                /* I/O  Pointer to Silk encoder state                                               */
    const silk_EncControlStruct     *encControl                             /* I    Control structure                                                           */
)
{
    const opus_int Complexity = encControl->complexity;
    const opus_int LBRR_in_previous_packet = psEncC->LBRR_enabled;

    /********************************************/
    /* Set encoding complexity                  */
    /********************************************/
    celt_assert( Complexity >= 1 && Complexity <= 10 );
    {
        const silk_complexity_setting *row = &silk_complexity_table[ silk_complexity_row[ Complexity ] ];
        psEncC->pitchEstimationComplexity       = row->pitchEstimationComplexity;
        psEncC->pitchEstimationThreshold_Q16    = row->pitchEstimationThreshold_Q16;
        psEncC->pitchEstimationLPCOrder         = row->pitchEstimationLPCOrder;
        psEncC->shapingLPCOrder                 = row->shapingLPCOrder;
        psEncC->la_shape                        = row->la_shape_ms * MAX_FS_KHZ;
        psEncC->nStatesDelayedDecision          = row->nStatesDelayedDecision;
        psEncC->useInterpolatedNLSFs            = row->useInterpolatedNLSFs;
        psEncC->NLSF_MSVQ_Survivors             = row->NLSF_MSVQ_Survivors;
        psEncC->warping_Q16                     = row->warped ? MAX_FS_KHZ * SILK_FIX_CONST( WARPING_MULTIPLIER, 16 ) : 0;
    }
    psEncC->shapeWinLength                  = SUB_FRAME_LENGTH_MS * MAX_FS_KHZ + 2 * psEncC->la_shape;
    psEncC->Complexity                      = Complexity;

    celt_assert( psEncC->pitchEstimationLPCOrder <= MAX_FIND_PITCH_LPC_ORDER );
    celt_assert( psEncC->shapingLPCOrder         <= MAX_SHAPE_LPC_ORDER      );
    celt_assert( psEncC->nStatesDelayedDecision  <= MAX_DEL_DEC_STATES       );
    celt_assert( psEncC->warping_Q16             <= 32767                    );
    celt_assert( psEncC->la_shape                <= LA_SHAPE_MAX             );
    celt_assert( psEncC->shapeWinLength          <= SHAPE_LPC_WIN_MAX        );

    /********************************************/
    /* Set packet loss rate measured by farend  */
    /********************************************/
    psEncC->PacketLoss_perc = encControl->packetLossPercentage;

    /********************************************/
    /* Set LBRR usage                           */
    /********************************************/
    psEncC->LBRR_enabled = encControl->LBRR_coded;
    if( psEncC->LBRR_enabled ) {
        /* Set gain increase for coding LBRR excitation */
        if( LBRR_in_previous_packet == 0 ) {
            /* Previous packet did not have LBRR, and was therefore coded at a higher bitrate */
            psEncC->LBRR_GainIncreases = 7;
        } else {
            psEncC->LBRR_GainIncreases = silk_max_int( 7 - silk_SMULWB( (opus_int32)psEncC->PacketLoss_perc, SILK_FIX_CONST( 0.2, 16 ) ), 3 );
        }
    }
}
