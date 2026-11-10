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

#include "define.h"
#include "API.h"
#include "control.h"
#include "typedef.h"
#include "stack_alloc.h"
#include "structs.h"
#include "tuning_parameters.h"
#include "main_FIX.h"
#include "resampler_private.h"

/* The scratch one silk_Encode call needs is the largest sum live along one of three paths:
   - silk_encode_frame_FIX: the rate loop's two NSQ state copies (the LBRR pass borrows the first), the pitch
     residual and the bitstream copy, then the larger of silk_NSQ_del_dec and silk_pitch_analysis_core
     (the autocorrelations of find_pitch_lags and noise_shape_analysis need under 1 kB);
   - silk_stereo_LR_to_MS;
   - the 20 ms input buffer plus one resampler batch. */
#define SILK_ENC_FRAME_BYTES ( SCRATCH_BYTES( 2, silk_nsq_state ) + \
    SCRATCH_BYTES( LA_PITCH_MAX + MAX_FRAME_LENGTH + LTP_MEM_LENGTH, opus_int16 ) + SCRATCH_BYTES( 1275, opus_uint8 ) )
#define SILK_ENC_RESAMPLER_BYTES SCRATCH_BYTES( RESAMPLER_MAX_BATCH_SIZE_IN + SILK_RESAMPLER_MAX_FIR_ORDER, opus_int32 )
/* One 20 ms frame of 48 kHz input per channel */
#define SILK_ENC_INPUT_SAMPLES ( MAX_API_FS_KHZ * MAX_FRAME_LENGTH_MS )
_Static_assert( SILK_ENCODER_SCRATCH_BYTES >= SILK_ENC_FRAME_BYTES + SILK_NSQ_SCRATCH_BYTES, "silk_NSQ_del_dec scratch" );
_Static_assert( SILK_ENCODER_SCRATCH_BYTES >= SILK_ENC_FRAME_BYTES + SILK_PITCH_SCRATCH_BYTES, "pitch analysis scratch" );
_Static_assert( SILK_ENCODER_SCRATCH_BYTES >= SCRATCH_BYTES( MAX_FRAME_LENGTH + 2, opus_int16 ) +
    4 * SCRATCH_BYTES( MAX_FRAME_LENGTH, opus_int16 ), "silk_stereo_LR_to_MS scratch" );
_Static_assert( SILK_ENCODER_SCRATCH_BYTES >= SCRATCH_BYTES( SILK_ENC_INPUT_SAMPLES, opus_int16 ) +
    SILK_ENC_RESAMPLER_BYTES, "input resampling scratch" );
_Static_assert( sizeof( opus_res ) == sizeof( opus_int16 ), "mono input is resampled as it is" );

/****************************************/
/* Encoder functions                    */
/****************************************/

opus_int silk_Get_Encoder_Size(                         /* O    Returns error code                              */
    opus_int                        *encSizeBytes,      /* O    Number of bytes in SILK encoder state           */
    opus_int                         channels           /* I    Number of channels                              */
)
{
    opus_int ret = SILK_NO_ERROR;

    *encSizeBytes = sizeof( silk_encoder );
    /* Skip second encoder state for mono. */
    if ( channels == 1 ) {
        *encSizeBytes -= sizeof( silk_encoder_state_Fxx );
    }

    return ret;
}

/*************************/
/* Init encoder          */
/*************************/
/* The state is zeroed by the caller (and the mono channel's second state is not part of it) */
void silk_InitEncoder(
    void                            *encState,          /* I/O  State, zeroed by the caller                     */
    int                              channels           /* I    Number of channels                              */
)
{
    silk_encoder *psEnc = (silk_encoder *)encState;
    opus_int n;

    for( n = 0; n < channels; n++ ) {
        silk_init_encoder( &psEnc->state_Fxx[ n ] );
    }

    psEnc->nChannelsInternal = 1;
}

/**************************/
/* Encode frame with Silk */
/**************************/
void silk_Encode(
    void                            *encState,          /* I/O  State                                           */
    silk_EncControlStruct           *encControl,        /* I    Control status                                  */
    const opus_res                  *samplesIn,         /* I    One 20 ms frame of 48 kHz PCM per channel       */
    ec_enc                          *psRangeEnc,        /* I/O  Compressor data structure                       */
    opus_int32                      *nBytesOut,         /* O    Number of bytes in payload                      */
    opus_int                        activity,           /* I    Decision of Opus voice activity detector        */
    char                            *scratch            /* I    Scratch arena                                   */
)
{
    opus_int   n, nBits, flags, curr_nBitsUsedLBRR;
    opus_int32 TargetRate_bps, MStargetRates_bps[ 2 ], channelRate_bps, sum;
    silk_encoder *psEnc = ( silk_encoder * )encState;
    silk_encoder_state_Fxx *ch0 = &psEnc->state_Fxx[ 0 ], *ch1 = &psEnc->state_Fxx[ 1 ];
    VARDECL( opus_int16, buf );
    opus_int transition;
    char *scratch_frame;
    SAVE_STACK;

    if( encControl->nChannelsInternal > psEnc->nChannelsInternal ) {
        /* Mono -> Stereo transition: init state of second channel and stereo state */
        silk_init_encoder( ch1 );
        silk_memset( psEnc->sStereo.pred_prev_Q13, 0, sizeof( psEnc->sStereo.pred_prev_Q13 ) );
        silk_memset( psEnc->sStereo.sSide, 0, sizeof( psEnc->sStereo.sSide ) );
        psEnc->sStereo.mid_side_amp_Q0[ 0 ] = 0;
        psEnc->sStereo.mid_side_amp_Q0[ 1 ] = 1;
        psEnc->sStereo.mid_side_amp_Q0[ 2 ] = 0;
        psEnc->sStereo.mid_side_amp_Q0[ 3 ] = 1;
        psEnc->sStereo.width_prev_Q14 = 0;
        psEnc->sStereo.smth_width_Q14 = SILK_FIX_CONST( 1, 14 );
    }

    transition = psEnc->nChannelsInternal != encControl->nChannelsInternal;

    psEnc->nChannelsInternal = encControl->nChannelsInternal;

    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        silk_control_encoder( &psEnc->state_Fxx[ n ].sCmn, encControl );
        if( psEnc->state_Fxx[ n ].sCmn.first_frame_after_reset || transition ) {
            psEnc->state_Fxx[ n ].sCmn.LBRR_pending = 0;
        }
    }

    /* Input resampling: the frame's 960 samples per channel become 320 at 16 kHz, after the two samples of
       look-back that silk_encode_frame_FIX and the VAD expect */
    scratch_frame = scratch;    /* The input buffer is dead once the frame is resampled into the encoder state */
    ALLOC( buf, SILK_ENC_INPUT_SAMPLES, opus_int16 );
    if( encControl->nChannelsAPI == 2 && encControl->nChannelsInternal == 2 ) {
        for( n = 0; n < SILK_ENC_INPUT_SAMPLES; n++ ) {
            buf[ n ] = RES2INT16(samplesIn[ 2 * n ]);
        }
        /* Making sure to start both resamplers from the same state when switching from mono to stereo */
        if( psEnc->nPrevChannelsInternal == 1 ) {
            silk_memcpy( &ch1->sCmn.resampler_state, &ch0->sCmn.resampler_state, sizeof( ch1->sCmn.resampler_state ) );
        }
        silk_resampler( &ch0->sCmn.resampler_state, &ch0->sCmn.inputBuf[ 2 ], buf, SILK_ENC_INPUT_SAMPLES, scratch );
        for( n = 0; n < SILK_ENC_INPUT_SAMPLES; n++ ) {
            buf[ n ] = RES2INT16(samplesIn[ 2 * n + 1 ]);
        }
        silk_resampler( &ch1->sCmn.resampler_state, &ch1->sCmn.inputBuf[ 2 ], buf, SILK_ENC_INPUT_SAMPLES, scratch );
    } else if( encControl->nChannelsAPI == 2 ) {
        /* Combine left and right channels before resampling */
        for( n = 0; n < SILK_ENC_INPUT_SAMPLES; n++ ) {
            sum = RES2INT16(samplesIn[ 2 * n ] + samplesIn[ 2 * n + 1 ]);
            buf[ n ] = (opus_int16)silk_RSHIFT_ROUND( sum,  1 );
        }
        silk_resampler( &ch0->sCmn.resampler_state, &ch0->sCmn.inputBuf[ 2 ], buf, SILK_ENC_INPUT_SAMPLES, scratch );
        /* On the first mono frame, average the results for the two resampler states  */
        if( psEnc->nPrevChannelsInternal == 2 ) {
            silk_resampler( &ch1->sCmn.resampler_state, &ch1->sCmn.inputBuf[ 2 ], buf, SILK_ENC_INPUT_SAMPLES, scratch );
            for( n = 0; n < MAX_FRAME_LENGTH; n++ ) {
                ch0->sCmn.inputBuf[ n + 2 ] = silk_RSHIFT( ch0->sCmn.inputBuf[ n + 2 ] + ch1->sCmn.inputBuf[ n + 2 ], 1 );
            }
        }
    } else {
        /* The 16-bit input is the resampler's input as it is (RES2INT16 is the identity) */
        silk_resampler( &ch0->sCmn.resampler_state, &ch0->sCmn.inputBuf[ 2 ], samplesIn, SILK_ENC_INPUT_SAMPLES, scratch );
    }

    /* Create space at start of payload for VAD and FEC flags */
    {
        opus_uint8 iCDF[ 2 ] = { 0, 0 };
        iCDF[ 0 ] = 256 - silk_RSHIFT( 256, 2 * encControl->nChannelsInternal );
        ec_enc_icdf( psRangeEnc, 0, iCDF, 8 );
    }
    curr_nBitsUsedLBRR = ec_tell( psRangeEnc );

    /* Encode any LBRR data from previous packet */
    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        psEnc->state_Fxx[ n ].sCmn.LBRR_flag = psEnc->state_Fxx[ n ].sCmn.LBRR_pending > 0 ? 1 : 0;
    }
    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        silk_encoder_state *psEncC = &psEnc->state_Fxx[ n ].sCmn;
        if( psEncC->LBRR_pending ) {
            if( encControl->nChannelsInternal == 2 && n == 0 ) {
                silk_stereo_encode_pred( psRangeEnc, psEnc->sStereo.predIx );
                /* For LBRR data there's no need to code the mid-only flag if the side-channel LBRR flag is set */
                if( ch1->sCmn.LBRR_pending == 0 ) {
                    silk_stereo_encode_mid_only( psRangeEnc, psEnc->sStereo.mid_only_flag );
                }
            }
            silk_encode_indices( psEncC, psRangeEnc, 1 );
            silk_encode_pulses( psRangeEnc, psEncC->indices_LBRR.signalType, psEncC->indices_LBRR.quantOffsetType,
                psEncC->pulses_LBRR );
        }
    }

    /* Reset LBRR flags */
    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        psEnc->state_Fxx[ n ].sCmn.LBRR_pending = 0;
    }
    curr_nBitsUsedLBRR = ec_tell( psRangeEnc ) - curr_nBitsUsedLBRR;

    silk_HP_variable_cutoff( psEnc->state_Fxx );

    /* Total target bits for packet */
    nBits = silk_DIV32_16( silk_MUL( encControl->bitRate, 20 ), 1000 );
    /* Subtract bits used for LBRR */
    /* psEnc->nBitsUsedLBRR is an exponential moving average of the LBRR usage,
       except that for the first LBRR frame it does no averaging and for the first
       frame after after LBRR, it goes back to zero immediately. */
    if ( curr_nBitsUsedLBRR < 10 ) {
        psEnc->nBitsUsedLBRR = 0;
    } else if ( psEnc->nBitsUsedLBRR < 10) {
        psEnc->nBitsUsedLBRR = curr_nBitsUsedLBRR;
    } else {
        psEnc->nBitsUsedLBRR = ( psEnc->nBitsUsedLBRR + curr_nBitsUsedLBRR ) / 2;
    }
    nBits -= psEnc->nBitsUsedLBRR;
    /* Convert to bits/second */
    TargetRate_bps = silk_SMULBB( nBits, 50 );
    /* Subtract fraction of bits in excess of target in previous frames and packets */
    TargetRate_bps -= silk_DIV32_16( silk_MUL( psEnc->nBitsExceeded, 1000 ), BITRESERVOIR_DECAY_TIME_MS );
    /* Never exceed input bitrate */
    TargetRate_bps = silk_LIMIT( TargetRate_bps, encControl->bitRate, 5000 );

    /* Convert Left/Right to Mid/Side */
    if( encControl->nChannelsInternal == 2 ) {
        silk_stereo_LR_to_MS( &psEnc->sStereo, &ch0->sCmn.inputBuf[ 2 ], &ch1->sCmn.inputBuf[ 2 ],
            psEnc->sStereo.predIx, &psEnc->sStereo.mid_only_flag,
            MStargetRates_bps, TargetRate_bps, ch0->sCmn.speech_activity_Q8, encControl->toMono, scratch_frame );
        if( psEnc->sStereo.mid_only_flag == 0 ) {
            /* Reset side channel encoder memory for first frame with side coding */
            if( psEnc->prev_decode_only_middle == 1 ) {
                silk_memset( &ch1->sShape,               0, sizeof( ch1->sShape ) );
                silk_memset( &ch1->sCmn.sNSQ,            0, sizeof( ch1->sCmn.sNSQ ) );
                silk_memset( ch1->sCmn.prev_NLSFq_Q15,   0, sizeof( ch1->sCmn.prev_NLSFq_Q15 ) );
                ch1->sCmn.prevLag                 = 100;
                ch1->sCmn.sNSQ.lagPrev            = 100;
                ch1->sShape.LastGainIndex         = 10;
                ch1->sCmn.prevSignalType          = TYPE_NO_VOICE_ACTIVITY;
                ch1->sCmn.sNSQ.prev_gain_Q16      = 65536;
                ch1->sCmn.first_frame_after_reset = 1;
            }
            silk_encode_do_VAD_Fxx( ch1, activity );
        } else {
            ch1->sCmn.VAD_flag = 0;
        }
        silk_stereo_encode_pred( psRangeEnc, psEnc->sStereo.predIx );
        if( ch1->sCmn.VAD_flag == 0 ) {
            silk_stereo_encode_mid_only( psRangeEnc, psEnc->sStereo.mid_only_flag );
        }
    } else {
        /* Buffering */
        silk_memcpy( ch0->sCmn.inputBuf, psEnc->sStereo.sMid, 2 * sizeof( opus_int16 ) );
        silk_memcpy( psEnc->sStereo.sMid, &ch0->sCmn.inputBuf[ MAX_FRAME_LENGTH ], 2 * sizeof( opus_int16 ) );
    }
    silk_encode_do_VAD_Fxx( ch0, activity );

    /* Encode */
    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        opus_int maxBits;

        /* Handling rate constraints */
        maxBits = encControl->maxBits;

        if( encControl->nChannelsInternal == 1 ) {
            channelRate_bps = TargetRate_bps;
        } else {
            channelRate_bps = MStargetRates_bps[ n ];
            if( n == 0 && MStargetRates_bps[ 1 ] > 0 ) {
                /* Give mid up to 1/2 of the max bits for that frame */
                maxBits -= encControl->maxBits / 2;
            }
        }

        if( channelRate_bps > 0 ) {
            silk_control_SNR( &psEnc->state_Fxx[ n ].sCmn, channelRate_bps );

            silk_encode_frame_Fxx( &psEnc->state_Fxx[ n ], nBytesOut, psRangeEnc, maxBits, scratch_frame );
        }
    }
    psEnc->prev_decode_only_middle = psEnc->sStereo.mid_only_flag;

    /* Insert VAD and FEC flags at beginning of bitstream */
    flags = 0;
    for( n = 0; n < encControl->nChannelsInternal; n++ ) {
        flags  = silk_LSHIFT( flags, 1 );
        flags |= psEnc->state_Fxx[ n ].sCmn.VAD_flag;
        flags  = silk_LSHIFT( flags, 1 );
        flags |= psEnc->state_Fxx[ n ].sCmn.LBRR_flag;
    }
    ec_enc_patch_initial_bits( psRangeEnc, flags, 2 * encControl->nChannelsInternal );

    psEnc->nBitsExceeded += *nBytesOut * 8;
    psEnc->nBitsExceeded -= silk_DIV32_16( silk_MUL( encControl->bitRate, 20 ), 1000 );
    psEnc->nBitsExceeded  = silk_LIMIT( psEnc->nBitsExceeded, 0, 10000 );

    psEnc->nPrevChannelsInternal = encControl->nChannelsInternal;

    encControl->stereoWidth_Q14 = encControl->toMono ? 0 : psEnc->sStereo.smth_width_Q14;

    encControl->signalType = ch0->sCmn.indices.signalType;
    encControl->offset = silk_Quantization_Offsets_Q10
                         [ ch0->sCmn.indices.signalType >> 1 ]
                         [ ch0->sCmn.indices.quantOffsetType ];
    RESTORE_STACK;
}
