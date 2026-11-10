/* Copyright (c) 2010-2011 Xiph.Org Foundation, Skype Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* maiopus: the encoder is reduced to one voice profile: 48 kHz, VOIP tuning,
   one 20 ms VBR frame per packet, 16-bit input, always coded as full-band
   hybrid (SILK wideband plus CELT bands 17-20): no CELT-only or SILK-only
   packets, no audio bandwidth switching, no mode-transition redundancy, no
   constrained-VBR CELT, complexity 1-10. For MaiCord's settings the packets
   equal those of the encoder before this reduction, which coded full-band
   hybrid there as upstream libopus's mode decision does. */

#include <stdarg.h>
#include "celt.h"
#include "entenc.h"
#include "modes.h"
#include "API.h"
#include "stack_alloc.h"
#include "float_cast.h"
#include "opus.h"
#include "arch.h"
#include "pitch.h"
#include "opus_private.h"
#include "os_support.h"
#include "mathops.h"
#include "tuning_parameters.h"


#include "fixed/structs_FIX.h"

#define VOICE_FS 48000
#define VOICE_FRAME_SIZE 960
#define VOICE_FRAME_RATE 50
/* 4 ms: 2.5 ms of SILK look-ahead + 1.5 ms for SILK resamplers and stereo prediction. */
#define DELAY_COMPENSATION 192
/* VOIP assumes 90% confidence of speech (Q7). */
#define VOICE_ESTIMATE 115
/* The only packet capacity (the RFC 6716 single-frame bound) and the settable bitrates, in bit/s. */
#define MAX_PACKET_BYTES 1275
#define MIN_BITRATE 6000
#define MAX_BITRATE 510000

struct OpusEncoder {
    int          celt_enc_offset;
    int          silk_enc_offset;
    silk_EncControlStruct silk_mode;
    int          channels;
    opus_int32   bitrate_bps;

    int          stream_channels;
    opus_int16   hybrid_stereo_width_Q14;
    opus_int32   variable_HP_smth2_Q15;
    opus_val16   prev_HB_gain;
    opus_val32   hp_mem[4];
    int          prev_channels;
    /* Needs to be the last field because it may be partially omitted (mono). */
    opus_res     delay_buffer[DELAY_COMPENSATION*2];
};

/* Threshold bit-rates for switching between mono and stereo */
static const opus_int32 stereo_voice_threshold = 19000;
static const opus_int32 stereo_music_threshold = 17000;

/* maiopus: opus_encode() scratch. opus_encode_frame_native keeps the delayed
   PCM frame, then runs SILK or CELT. */
#define OPUS_ENCODE_FRAME_SCRATCH_BYTES \
    SCRATCH_BYTES((DELAY_COMPENSATION+VOICE_FRAME_SIZE)*2, opus_res)
_Static_assert(OPUS_ENCODE_FRAME_SCRATCH_BYTES+CELT_ENCODER_SCRATCH_BYTES <= OPUS_SCRATCH_BYTES &&
               OPUS_ENCODE_FRAME_SCRATCH_BYTES+SILK_ENCODER_SCRATCH_BYTES <= OPUS_SCRATCH_BYTES,
               "the encoder's worst-case scratch exceeds OPUS_SCRATCH_BYTES");

int opus_encoder_get_size(int channels)
{
    int ret;
    ret = opus_encoder_init(NULL, VOICE_FS, channels, OPUS_APPLICATION_VOIP);
    if (ret < 0)
        return 0;
    else
        return ret;
}

int opus_encoder_init(OpusEncoder* st, opus_int32 Fs, int channels, int application)
{
    void *silk_enc;
    CELTEncoder *celt_enc;
    int err;
    int ret, silkEncSizeBytes, celtEncSizeBytes;
    int tot_size;
    int base_size;

    if (Fs != VOICE_FS || (channels!=1&&channels!=2) || application != OPUS_APPLICATION_VOIP)
        return OPUS_BAD_ARG;

    /* Create SILK encoder */
    ret = silk_Get_Encoder_Size( &silkEncSizeBytes, channels );
    if (ret)
        return OPUS_BAD_ARG;
    silkEncSizeBytes = align(silkEncSizeBytes);
    celtEncSizeBytes = celt_encoder_get_size(channels);
    base_size = align(sizeof(OpusEncoder));
    if (channels==1)
       base_size = align(base_size - DELAY_COMPENSATION*sizeof(opus_res));
    tot_size = base_size+silkEncSizeBytes+celtEncSizeBytes;
    if (st == NULL) {
        return tot_size;
    }
    OPUS_CLEAR((char*)st, tot_size);
    st->silk_enc_offset = base_size;
    st->celt_enc_offset = st->silk_enc_offset+silkEncSizeBytes;

    st->stream_channels = st->channels = channels;

    silk_enc = (char*)st+st->silk_enc_offset;
    ret = silk_InitEncoder( silk_enc, st->channels, &st->silk_mode );
    if(ret)return OPUS_INTERNAL_ERROR;

    /* default SILK parameters: the internal rate is always 16 kHz (wideband), packets 20 ms */
    st->silk_mode.nChannelsAPI              = channels;
    st->silk_mode.nChannelsInternal         = channels;
    st->silk_mode.API_sampleRate            = VOICE_FS;
    st->silk_mode.payloadSize_ms            = 20;
    st->silk_mode.bitRate                   = 25000;
    st->silk_mode.packetLossPercentage      = 0;
    st->silk_mode.complexity                = 9;
    st->silk_mode.useInBandFEC              = 0;
    st->silk_mode.useDRED                   = 0;
    st->silk_mode.useCBR                    = 0;

    /* Create CELT encoder */
    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);
    err = celt_encoder_init(celt_enc, channels);
    if(err!=OPUS_OK)return OPUS_INTERNAL_ERROR;
    celt_encoder_ctl(celt_enc, OPUS_SET_COMPLEXITY(st->silk_mode.complexity));

    /* The bitrate OPUS_AUTO would have chosen. */
    st->bitrate_bps = 60*VOICE_FRAME_RATE + VOICE_FS*channels;

    st->hybrid_stereo_width_Q14 = 1 << 14;
    st->prev_HB_gain = Q15ONE;
    st->variable_HP_smth2_Q15 = silk_LSHIFT( silk_lin2log( VARIABLE_HP_MIN_CUTOFF_HZ ), 8 );

    return OPUS_OK;
}

static void hp_cutoff(const opus_res *in, opus_int32 cutoff_Hz, opus_res *out, opus_val32 *hp_mem, int len, int channels)
{
   opus_int32 B_Q28[ 3 ], A_Q28[ 2 ];
   opus_int32 Fc_Q19, r_Q28, r_Q22;

   silk_assert( cutoff_Hz <= silk_int32_MAX / SILK_FIX_CONST( 1.5 * 3.14159 / 1000, 19 ) );
   Fc_Q19 = silk_DIV32_16( silk_SMULBB( SILK_FIX_CONST( 1.5 * 3.14159 / 1000, 19 ), cutoff_Hz ), VOICE_FS/1000 );
   silk_assert( Fc_Q19 > 0 && Fc_Q19 < 32768 );

   r_Q28 = SILK_FIX_CONST( 1.0, 28 ) - silk_MUL( SILK_FIX_CONST( 0.92, 9 ), Fc_Q19 );

   /* b = r * [ 1; -2; 1 ]; */
   /* a = [ 1; -2 * r * ( 1 - 0.5 * Fc^2 ); r^2 ]; */
   B_Q28[ 0 ] = r_Q28;
   B_Q28[ 1 ] = silk_LSHIFT( -r_Q28, 1 );
   B_Q28[ 2 ] = r_Q28;

   /* -r * ( 2 - Fc * Fc ); */
   r_Q22  = silk_RSHIFT( r_Q28, 6 );
   A_Q28[ 0 ] = silk_SMULWW( r_Q22, silk_SMULWW( Fc_Q19, Fc_Q19 ) - SILK_FIX_CONST( 2.0,  22 ) );
   A_Q28[ 1 ] = silk_SMULWW( r_Q22, r_Q22 );

   if( channels == 1 ) {
      silk_biquad_alt_stride1( in, B_Q28, A_Q28, hp_mem, out, len );
   } else {
      silk_biquad_alt_stride2( in, B_Q28, A_Q28, hp_mem, out, len );
   }
}

static void stereo_fade(const opus_res *in, opus_res *out, opus_val16 g1, opus_val16 g2,
        int overlap, int frame_size, int channels, const celt_coef *window)
{
    int i;
    g1 = Q15ONE-g1;
    g2 = Q15ONE-g2;
    for (i=0;i<overlap;i++)
    {
       opus_val32 diff;
       opus_val16 g, w;
       w = COEF2VAL16(window[i]);
       w = MULT16_16_Q15(w, w);
       g = SHR32(MAC16_16(MULT16_16(w,g2),
             Q15ONE-w, g1), 15);
       diff = HALF32((opus_val32)in[i*channels] - (opus_val32)in[i*channels+1]);
       diff = MULT16_RES_Q15(g, diff);
       out[i*channels] = out[i*channels] - diff;
       out[i*channels+1] = out[i*channels+1] + diff;
    }
    for (;i<frame_size;i++)
    {
       opus_val32 diff;
       diff = HALF32((opus_val32)in[i*channels] - (opus_val32)in[i*channels+1]);
       diff = MULT16_RES_Q15(g2, diff);
       out[i*channels] = out[i*channels] - diff;
       out[i*channels+1] = out[i*channels+1] + diff;
    }
}

static void gain_fade(const opus_res *in, opus_res *out, opus_val16 g1, opus_val16 g2,
        int overlap, int frame_size, int channels, const celt_coef *window)
{
    int i;
    int c;
    if (channels==1)
    {
       for (i=0;i<overlap;i++)
       {
          opus_val16 g, w;
          w = COEF2VAL16(window[i]);
          w = MULT16_16_Q15(w, w);
          g = SHR32(MAC16_16(MULT16_16(w,g2),
                Q15ONE-w, g1), 15);
          out[i] = MULT16_RES_Q15(g, in[i]);
       }
    } else {
       for (i=0;i<overlap;i++)
       {
          opus_val16 g, w;
          w = COEF2VAL16(window[i]);
          w = MULT16_16_Q15(w, w);
          g = SHR32(MAC16_16(MULT16_16(w,g2),
                Q15ONE-w, g1), 15);
          out[i*2] = MULT16_RES_Q15(g, in[i*2]);
          out[i*2+1] = MULT16_RES_Q15(g, in[i*2+1]);
       }
    }
    c=0;do {
       for (i=overlap;i<frame_size;i++)
       {
          out[i*channels+c] = MULT16_RES_Q15(g2, in[i*channels+c]);
       }
    }
    while (++c<channels);
}

/* In-band FEC is enabled when the rate covers the full-band threshold, adjusted for the expected loss,
   with hysteresis around the previous decision. */
static int decide_fec(int useInBandFEC, int PacketLoss_perc, int last_fec, opus_int32 rate)
{
   opus_int32 LBRR_rate_thres_bps;
   if (!useInBandFEC || PacketLoss_perc == 0)
      return 0;
   LBRR_rate_thres_bps = 22000;
   if (last_fec == 1) LBRR_rate_thres_bps -= 1000;
   if (last_fec == 0) LBRR_rate_thres_bps += 1000;
   LBRR_rate_thres_bps = silk_SMULWB( silk_MUL( LBRR_rate_thres_bps,
         125 - silk_min( PacketLoss_perc, 25 ) ), SILK_FIX_CONST( 0.01, 16 ) );
   return rate > LBRR_rate_thres_bps;
}

static int compute_silk_rate_for_hybrid(int rate, int fec, int channels) {
   int entry;
   int i;
   int N;
   int silk_rate;
   static const int rate_table[][3] = {
  /*  |total| |-- SILK, 20 ms --|
               No FEC     FEC  */
      {    0,     0,     0},
      {12000, 10000, 11000},
      {16000, 13500, 15000},
      {20000, 16000, 18000},
      {24000, 18000, 21000},
      {32000, 22000, 28000},
      {64000, 38000, 50000}
   };
   /* Do the allocation per-channel. */
   rate /= channels;
   entry = 1 + fec;
   N = sizeof(rate_table)/sizeof(rate_table[0]);
   for (i=1;i<N;i++)
   {
      if (rate_table[i][0] > rate) break;
   }
   if (i == N)
   {
      silk_rate = rate_table[i-1][entry];
      /* For now, just give 50% of the extra bits to SILK. */
      silk_rate += (rate-rate_table[i-1][0])/2;
   } else {
      opus_int32 lo, hi, x0, x1;
      lo = rate_table[i-1][entry];
      hi = rate_table[i][entry];
      x0 = rate_table[i-1][0];
      x1 = rate_table[i][0];
      silk_rate = (lo*(x1-rate) + hi*(rate-x0))/(x1-x0);
   }
   silk_rate *= channels;
   /* Small adjustment for stereo (calibrated for 32 kb/s, haven't tried other bitrates). */
   if (channels == 2 && rate >= 12000)
      silk_rate -= 1000;
   return silk_rate;
}

/* Returns the equivalent bitrate corresponding to 20 ms frames,
   complexity 10 VBR operation. `hybrid` is zero before the stream's channel
   count is decided. */
static opus_int32 compute_equiv_rate(opus_int32 bitrate, int hybrid, int complexity, int loss)
{
   opus_int32 equiv;
   equiv = bitrate;
   /* Complexity makes about 10% difference (from 0 to 10) in general. */
   equiv = equiv * (90+complexity)/100;
   if (hybrid)
   {
      /* SILK complexity 1 runs a single delayed-decision state, which
         costs about 20%. */
      if (complexity<2)
         equiv = equiv*4/5;
      equiv -= equiv*loss/(6*loss + 10);
   } else {
      /* Half the SILK loss*/
      equiv -= equiv*loss/(12*loss + 20);
   }
   return equiv;
}

static int is_digital_silence(const opus_res* pcm, int channels)
{
   return celt_maxabs_res(pcm, VOICE_FRAME_SIZE*channels) == 0;
}

opus_int32 opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size,
                unsigned char *data, opus_int32 max_data_bytes, void *scratch_block)
{
    char *scratch = (char*)scratch_block;
    void *silk_enc;
    CELTEncoder *celt_enc;
    const CELTMode *celt_mode=NULL;
    int ret=0;
    opus_int32 nBytes;
    ec_enc enc;
    int bits_target;
    int nb_compr_bytes;
    int cutoff_Hz;
    int hp_freq_smth1;
    opus_val16 HB_gain;
    opus_int32 equiv_rate;
    opus_int32 total_bitRate, celt_rate, maxBitRate;
    opus_int activity = VAD_NO_DECISION;
    SILKInfo info;
    VARDECL(opus_res, pcm_buf);
    SAVE_STACK;

    if (frame_size != VOICE_FRAME_SIZE || max_data_bytes != MAX_PACKET_BYTES)
       return OPUS_BAD_ARG;

    silk_enc = (char*)st+st->silk_enc_offset;
    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);
    celt_encoder_ctl(celt_enc, CELT_GET_MODE(&celt_mode));

    if (is_digital_silence(pcm, st->channels))
       activity = 0;

    /* Equivalent 20-ms rate for the mono-stereo decision */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, 0, st->silk_mode.complexity, st->silk_mode.packetLossPercentage);

    /* Rate-dependent mono-stereo decision */
    if (st->channels == 2)
    {
       opus_int32 stereo_threshold;
       stereo_threshold = stereo_music_threshold + ((VOICE_ESTIMATE*VOICE_ESTIMATE*(stereo_voice_threshold-stereo_music_threshold))>>14);
       if (st->stream_channels == 2)
          stereo_threshold -= 1000;
       else
          stereo_threshold += 1000;
       st->stream_channels = (equiv_rate > stereo_threshold) ? 2 : 1;
    } else {
       st->stream_channels = st->channels;
    }

    if (st->stream_channels == 1 && st->prev_channels ==2 && st->silk_mode.toMono==0)
    {
       /* Delay stereo->mono transition by two frames so that SILK can do a smooth downmix */
       st->silk_mode.toMono = 1;
       st->stream_channels = 2;
    } else {
       st->silk_mode.toMono = 0;
    }

    /* Equivalent rate with the hybrid mode. */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, 1, st->silk_mode.complexity, st->silk_mode.packetLossPercentage);

    st->silk_mode.LBRR_coded = decide_fec(st->silk_mode.useInBandFEC, st->silk_mode.packetLossPercentage,
          st->silk_mode.LBRR_coded, equiv_rate);

    /* The bitrate setting is at most 510 kb/s, the rate of a full packet. */
    bits_target = bitrate_to_bits(st->bitrate_bps, VOICE_FS, VOICE_FRAME_SIZE) - 8;

    ec_enc_init(&enc, data+1, MAX_PACKET_BYTES-1);

    ALLOC(pcm_buf, (DELAY_COMPENSATION+VOICE_FRAME_SIZE)*st->channels, opus_res);
    OPUS_COPY(pcm_buf, st->delay_buffer, DELAY_COMPENSATION*st->channels);

    hp_freq_smth1 = ((silk_encoder*)silk_enc)->state_Fxx[0].sCmn.variable_HP_smth1_Q15;

    st->variable_HP_smth2_Q15 = silk_SMLAWB( st->variable_HP_smth2_Q15,
          hp_freq_smth1 - st->variable_HP_smth2_Q15, SILK_FIX_CONST( VARIABLE_HP_SMTH_COEF2, 16 ) );

    /* convert from log scale to Hertz */
    cutoff_Hz = silk_log2lin( silk_RSHIFT( st->variable_HP_smth2_Q15, 8 ) );

    hp_cutoff(pcm, cutoff_Hz, &pcm_buf[DELAY_COMPENSATION*st->channels], st->hp_mem, VOICE_FRAME_SIZE, st->channels);

    /* Distribute bits between SILK and CELT */
    total_bitRate = bits_to_bitrate(bits_target, VOICE_FS, VOICE_FRAME_SIZE);
    /* Base rate for SILK */
    st->silk_mode.bitRate = compute_silk_rate_for_hybrid(total_bitRate,
          st->silk_mode.LBRR_coded, st->stream_channels);
    /* Increasingly attenuate high band when it gets allocated fewer bits */
    celt_rate = total_bitRate - st->silk_mode.bitRate;
    HB_gain = Q15ONE - SHR32(celt_exp2(-celt_rate * QCONST16(1.f/1024, 10)), 1);

    st->silk_mode.nChannelsInternal = st->stream_channels;

    /* Call SILK encoder for the low band */

    /* Max bits for SILK, counting ToC. */
    st->silk_mode.maxBits = (MAX_PACKET_BYTES-1)*8;
    /* Constrained VBR: the SILK bitrate corresponding to the max total bits available */
    maxBitRate = compute_silk_rate_for_hybrid(st->silk_mode.maxBits*VOICE_FRAME_RATE,
          st->silk_mode.LBRR_coded, st->stream_channels);
    st->silk_mode.maxBits = bitrate_to_bits(maxBitRate, VOICE_FS, VOICE_FRAME_SIZE);

    ret = silk_Encode( silk_enc, &st->silk_mode, pcm_buf+DELAY_COMPENSATION*st->channels, VOICE_FRAME_SIZE, &enc, &nBytes, activity, scratch );
    if( ret ) {
       RESTORE_STACK;
       return OPUS_INTERNAL_ERROR;
    }

    celt_encoder_ctl(celt_enc, CELT_SET_CHANNELS(st->stream_channels));

    OPUS_COPY(st->delay_buffer, &pcm_buf[VOICE_FRAME_SIZE*st->channels], DELAY_COMPENSATION*st->channels);
    /* gain_fade() and stereo_fade() need to be after the buffer copying
       because we don't want any of this to affect the SILK part */
    if( st->prev_HB_gain < Q15ONE || HB_gain < Q15ONE ) {
       gain_fade(pcm_buf, pcm_buf,
             st->prev_HB_gain, HB_gain, celt_mode->overlap, VOICE_FRAME_SIZE, st->channels, celt_mode->window);
    }
    st->prev_HB_gain = HB_gain;
    if (st->stream_channels==1)
    {
       if (equiv_rate > 32000)
          st->silk_mode.stereoWidth_Q14 = 16384;
       else if (equiv_rate < 16000)
          st->silk_mode.stereoWidth_Q14 = 0;
       else
          st->silk_mode.stereoWidth_Q14 = 16384 - 2048*(opus_int32)(32000-equiv_rate)/(equiv_rate-14000);
    }
    if( st->channels == 2 ) {
        /* Apply stereo width reduction (at low bitrates) */
        if( st->hybrid_stereo_width_Q14 < (1 << 14) || st->silk_mode.stereoWidth_Q14 < (1 << 14) ) {
            opus_val16 g1, g2;
            g1 = st->hybrid_stereo_width_Q14;
            g2 = (opus_val16)(st->silk_mode.stereoWidth_Q14);
            g1 = g1==16384 ? Q15ONE : SHL16(g1,1);
            g2 = g2==16384 ? Q15ONE : SHL16(g2,1);
            stereo_fade(pcm_buf, pcm_buf, g1, g2, celt_mode->overlap,
                  VOICE_FRAME_SIZE, st->channels, celt_mode->window);
            st->hybrid_stereo_width_Q14 = st->silk_mode.stereoWidth_Q14;
        }
    }

    /* The flag for a redundant CELT frame, which is never sent */
    if (ec_tell(&enc)+17+20 <= 8*(MAX_PACKET_BYTES-1))
        ec_enc_bit_logp(&enc, 0, 12);

    nb_compr_bytes = MAX_PACKET_BYTES-1;
    ec_enc_shrink(&enc, nb_compr_bytes);

    info.signalType = st->silk_mode.signalType;
    info.offset = st->silk_mode.offset;
    celt_encoder_ctl(celt_enc, CELT_SET_SILK_INFO(&info));
    celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(st->bitrate_bps-st->silk_mode.bitRate));

    /* If false, we already busted the budget and we'll end up with a "PLC frame" */
    if (ec_tell(&enc) <= 8*nb_compr_bytes)
    {
       ret = celt_encode_with_ec(celt_enc, pcm_buf, nb_compr_bytes, &enc, scratch);
       if (ret < 0)
       {
          RESTORE_STACK;
          return OPUS_INTERNAL_ERROR;
       }
    }

    /* Signalling the mode in the first byte: 20 ms full-band hybrid */
    data[0] = 0x78 | ((st->stream_channels==2)<<2);

    st->prev_channels = st->stream_channels;

    /* In the unlikely case that the SILK encoder busted its target, tell
       the decoder to call the PLC */
    if (ec_tell(&enc) > (MAX_PACKET_BYTES-1)*8)
    {
       data[1] = 0;
       ret = 1;
    }
    /* Count ToC */
    ret += 1;
    RESTORE_STACK;
    return ret;
}

int opus_encoder_ctl(OpusEncoder *st, int request, ...)
{
    int ret;
    CELTEncoder *celt_enc;
    va_list ap;

    ret = OPUS_OK;
    va_start(ap, request);

    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);

    switch (request)
    {
        case OPUS_SET_BITRATE_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if (value < MIN_BITRATE || value > MAX_BITRATE)
            {
                goto bad_arg;
            }
            st->bitrate_bps = value;
        }
        break;
        case OPUS_SET_COMPLEXITY_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<1 || value>10)
            {
               goto bad_arg;
            }
            st->silk_mode.complexity = value;
            celt_encoder_ctl(celt_enc, OPUS_SET_COMPLEXITY(value));
        }
        break;
        case OPUS_SET_INBAND_FEC_REQUEST:
        {
            /* Mode 2 equals 1 under the VOIP speech estimate. */
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<0 || value>1)
            {
               goto bad_arg;
            }
            st->silk_mode.useInBandFEC = value;
        }
        break;
        case OPUS_SET_PACKET_LOSS_PERC_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if (value < 0 || value > 100)
            {
               goto bad_arg;
            }
            st->silk_mode.packetLossPercentage = value;
            celt_encoder_ctl(celt_enc, OPUS_SET_PACKET_LOSS_PERC(value));
        }
        break;
        case OPUS_SET_VBR_CONSTRAINT_REQUEST:
        {
            /* Hybrid coding is always constrained by the SILK bit limit; the setting has no effect. */
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<0 || value>1)
            {
               goto bad_arg;
            }
        }
        break;
        case OPUS_GET_LOOKAHEAD_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = VOICE_FS/400 + DELAY_COMPENSATION;
        }
        break;
        default:
            ret = OPUS_UNIMPLEMENTED;
            break;
    }
    va_end(ap);
    return ret;
bad_arg:
    va_end(ap);
    return OPUS_BAD_ARG;
}
