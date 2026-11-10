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
#define silk_encoder_state_Fxx      silk_encoder_state_FIX
#include "stack_alloc.h"
#include "tuning_parameters.h"
#include "pitch_est_defines.h"

static void silk_setup_fs(
    silk_encoder_state_Fxx          *psEnc              /* I/O                      */
);

static opus_int silk_setup_complexity(
    silk_encoder_state              *psEncC,            /* I/O                      */
    opus_int                        Complexity          /* I                        */
);

static OPUS_INLINE opus_int silk_setup_LBRR(
    silk_encoder_state              *psEncC,            /* I/O                      */
    const silk_EncControlStruct     *encControl         /* I                        */
);


/* Control encoder */
opus_int silk_control_encoder(
    silk_encoder_state_Fxx          *psEnc,                                 /* I/O  Pointer to Silk encoder state                                               */
    silk_EncControlStruct           *encControl,                            /* I    Control structure                                                           */
    const opus_int                  channelNb                               /* I    Channel number                                                              */
)
{
    opus_int   ret = 0;

    psEnc->sCmn.useCBR                 = encControl->useCBR;
    psEnc->sCmn.API_fs_Hz              = encControl->API_sampleRate;
    psEnc->sCmn.useInBandFEC           = encControl->useInBandFEC;
    psEnc->sCmn.nChannelsAPI           = encControl->nChannelsAPI;
    psEnc->sCmn.nChannelsInternal      = encControl->nChannelsInternal;
    psEnc->sCmn.channelNb              = channelNb;

    if( psEnc->sCmn.controlled_since_last_payload != 0 ) {
        return ret;
    }

    /* Beyond this point we know that there are no previously coded frames in the payload buffer */

    if( psEnc->sCmn.fs_kHz == 0 ) {
        /* A newly initialized state: prepare the resampler from API_fs_Hz to the 16 kHz internal rate */
        ret += silk_resampler_init( &psEnc->sCmn.resampler_state, psEnc->sCmn.API_fs_Hz, 16000, 1 );
        silk_setup_fs( psEnc );
    }

    /********************************************/
    /* Set encoding complexity                  */
    /********************************************/
    ret += silk_setup_complexity( &psEnc->sCmn, encControl->complexity  );

    /********************************************/
    /* Set packet loss rate measured by farend  */
    /********************************************/
    psEnc->sCmn.PacketLoss_perc = encControl->packetLossPercentage;

    /********************************************/
    /* Set LBRR usage                           */
    /********************************************/
    ret += silk_setup_LBRR( &psEnc->sCmn, encControl );

    psEnc->sCmn.controlled_since_last_payload = 1;

    return ret;
}

/* The internal rate is always 16 kHz and a packet always one 20 ms frame: set once on a cleared state */
static void silk_setup_fs(
    silk_encoder_state_Fxx          *psEnc              /* I/O                      */
)
{
    psEnc->sCmn.nFramesPerPacket            = 1;
    psEnc->sCmn.nb_subfr                    = MAX_NB_SUBFR;
    psEnc->sCmn.PacketSize_ms               = 20;
    psEnc->sCmn.pitch_contour_iCDF          = silk_pitch_contour_iCDF;
    psEnc->sCmn.TargetRate_bps              = 0;     /* trigger new SNR computation */

    /* Initialize non-zero parameters */
    psEnc->sCmn.prevLag                     = 100;
    psEnc->sCmn.first_frame_after_reset     = 1;
    psEnc->sShape.LastGainIndex             = 10;
    psEnc->sCmn.sNSQ.lagPrev                = 100;
    psEnc->sCmn.sNSQ.prev_gain_Q16          = 65536;
    psEnc->sCmn.prevSignalType              = TYPE_NO_VOICE_ACTIVITY;

    psEnc->sCmn.fs_kHz                      = 16;
    psEnc->sCmn.predictLPCOrder             = MAX_LPC_ORDER;
    psEnc->sCmn.psNLSF_CB                   = &silk_NLSF_CB_WB;
    psEnc->sCmn.subfr_length                = SUB_FRAME_LENGTH_MS * 16;
    psEnc->sCmn.frame_length                = psEnc->sCmn.subfr_length * MAX_NB_SUBFR;
    psEnc->sCmn.ltp_mem_length              = LTP_MEM_LENGTH_MS * 16;
    psEnc->sCmn.la_pitch                    = LA_PITCH_MS * 16;
    psEnc->sCmn.max_pitch_lag               = 18 * 16;
    psEnc->sCmn.pitch_LPC_win_length        = FIND_PITCH_LPC_WIN_MS * 16;
    psEnc->sCmn.pitch_lag_low_bits_iCDF     = silk_uniform8_iCDF;

    /* Check that settings are valid */
    celt_assert( ( psEnc->sCmn.subfr_length * psEnc->sCmn.nb_subfr ) == psEnc->sCmn.frame_length );
}

static opus_int silk_setup_complexity(
    silk_encoder_state              *psEncC,            /* I/O                      */
    opus_int                        Complexity          /* I                        */
)
{
    opus_int ret = 0;

    /* Set encoding complexity */
    celt_assert( Complexity >= 1 && Complexity <= 10 );
    if( Complexity < 2 ) {
        psEncC->pitchEstimationComplexity       = SILK_PE_MID_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.76, 16 );
        psEncC->pitchEstimationLPCOrder         = 8;
        psEncC->shapingLPCOrder                 = 14;
        psEncC->la_shape                        = 5 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = 1;
        psEncC->useInterpolatedNLSFs            = 0;
        psEncC->NLSF_MSVQ_Survivors             = 3;
        psEncC->warping_Q16                     = 0;
    } else if( Complexity < 3 ) {
        psEncC->pitchEstimationComplexity       = SILK_PE_MIN_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.8, 16 );
        psEncC->pitchEstimationLPCOrder         = 6;
        psEncC->shapingLPCOrder                 = 12;
        psEncC->la_shape                        = 3 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = 2;
        psEncC->useInterpolatedNLSFs            = 0;
        psEncC->NLSF_MSVQ_Survivors             = 2;
        psEncC->warping_Q16                     = 0;
    } else if( Complexity < 4 ) {
        psEncC->pitchEstimationComplexity       = SILK_PE_MID_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.76, 16 );
        psEncC->pitchEstimationLPCOrder         = 8;
        psEncC->shapingLPCOrder                 = 14;
        psEncC->la_shape                        = 5 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = 2;
        psEncC->useInterpolatedNLSFs            = 0;
        psEncC->NLSF_MSVQ_Survivors             = 4;
        psEncC->warping_Q16                     = 0;
    } else if( Complexity < 6 ) {
        psEncC->pitchEstimationComplexity       = SILK_PE_MID_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.74, 16 );
        psEncC->pitchEstimationLPCOrder         = 10;
        psEncC->shapingLPCOrder                 = 16;
        psEncC->la_shape                        = 5 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = 2;
        psEncC->useInterpolatedNLSFs            = 1;
        psEncC->NLSF_MSVQ_Survivors             = 6;
        psEncC->warping_Q16                     = psEncC->fs_kHz * SILK_FIX_CONST( WARPING_MULTIPLIER, 16 );
    } else if( Complexity < 8 ) {
        psEncC->pitchEstimationComplexity       = SILK_PE_MID_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.72, 16 );
        psEncC->pitchEstimationLPCOrder         = 12;
        psEncC->shapingLPCOrder                 = 20;
        psEncC->la_shape                        = 5 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = 3;
        psEncC->useInterpolatedNLSFs            = 1;
        psEncC->NLSF_MSVQ_Survivors             = 8;
        psEncC->warping_Q16                     = psEncC->fs_kHz * SILK_FIX_CONST( WARPING_MULTIPLIER, 16 );
    } else {
        psEncC->pitchEstimationComplexity       = SILK_PE_MAX_COMPLEX;
        psEncC->pitchEstimationThreshold_Q16    = SILK_FIX_CONST( 0.7, 16 );
        psEncC->pitchEstimationLPCOrder         = 16;
        psEncC->shapingLPCOrder                 = 24;
        psEncC->la_shape                        = 5 * psEncC->fs_kHz;
        psEncC->nStatesDelayedDecision          = MAX_DEL_DEC_STATES;
        psEncC->useInterpolatedNLSFs            = 1;
        psEncC->NLSF_MSVQ_Survivors             = 16;
        psEncC->warping_Q16                     = psEncC->fs_kHz * SILK_FIX_CONST( WARPING_MULTIPLIER, 16 );
    }

    /* Do not allow higher pitch estimation LPC order than predict LPC order */
    psEncC->pitchEstimationLPCOrder = silk_min_int( psEncC->pitchEstimationLPCOrder, psEncC->predictLPCOrder );
    psEncC->shapeWinLength          = SUB_FRAME_LENGTH_MS * psEncC->fs_kHz + 2 * psEncC->la_shape;
    psEncC->Complexity              = Complexity;

    celt_assert( psEncC->pitchEstimationLPCOrder <= MAX_FIND_PITCH_LPC_ORDER );
    celt_assert( psEncC->shapingLPCOrder         <= MAX_SHAPE_LPC_ORDER      );
    celt_assert( psEncC->nStatesDelayedDecision  <= MAX_DEL_DEC_STATES       );
    celt_assert( psEncC->warping_Q16             <= 32767                    );
    celt_assert( psEncC->la_shape                <= LA_SHAPE_MAX             );
    celt_assert( psEncC->shapeWinLength          <= SHAPE_LPC_WIN_MAX        );

    return ret;
}

static OPUS_INLINE opus_int silk_setup_LBRR(
    silk_encoder_state          *psEncC,            /* I/O                      */
    const silk_EncControlStruct *encControl         /* I                        */
)
{
    opus_int   LBRR_in_previous_packet, ret = SILK_NO_ERROR;

    LBRR_in_previous_packet = psEncC->LBRR_enabled;
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

    return ret;
}
