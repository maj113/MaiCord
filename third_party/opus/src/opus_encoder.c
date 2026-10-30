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
   one 20 ms VBR frame per packet, 16-bit input. For those settings it produces
   the same packets as upstream libopus. */

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
#include "cpu_support.h"
#include "mathops.h"
#include "tuning_parameters.h"


#include "fixed/structs_FIX.h"

#define VOICE_FS 48000
#define VOICE_FRAME_SIZE 960
#define VOICE_FRAME_RATE 50
/* SILK prefill and CELT history: 10 ms. */
#define MAX_ENCODER_BUFFER 480
/* 4 ms: 2.5 ms of SILK look-ahead + 1.5 ms for SILK resamplers and stereo prediction. */
#define DELAY_COMPENSATION 192
/* VOIP assumes 90% confidence of speech (Q7). */
#define VOICE_ESTIMATE 115

#define PSEUDO_SNR_THRESHOLD 316.23f    /* 10^(25/10) */

typedef struct {
   opus_val32 XX, XY, YY;
   opus_val16 smoothed_width;
   opus_val16 max_follower;
} StereoWidthState;

struct OpusEncoder {
    int          celt_enc_offset;
    int          silk_enc_offset;
    silk_EncControlStruct silk_mode;
    int          channels;
    int          vbr_constraint;
    opus_int32   bitrate_bps;
    opus_int32   user_bitrate_bps;
    int          arch;
    int          use_dtx;                 /* general DTX for both SILK and CELT */

#define OPUS_ENCODER_RESET_START stream_channels
    int          stream_channels;
    opus_int16   hybrid_stereo_width_Q14;
    opus_int32   variable_HP_smth2_Q15;
    opus_val16   prev_HB_gain;
    opus_val32   hp_mem[4];
    int          mode;
    int          prev_mode;
    int          prev_channels;
    int          bandwidth;
    /* Bandwidth determined automatically from the rate (before any other adjustment) */
    int          auto_bandwidth;
    int          silk_bw_switch;
    int          first;
    StereoWidthState width_mem;
    int          nb_no_activity_ms_Q1;
    opus_val32   peak_signal_energy;
    opus_uint32  rangeFinal;
    /* Needs to be the last field because it may be partially omitted (mono). */
    opus_res     delay_buffer[MAX_ENCODER_BUFFER*2];
};

/* Transition tables for the voice and music (the same for mono and stereo).
   First column is the middle (memoriless) threshold. The second column is the
   hysteresis (difference with the middle) */
static const opus_int32 voice_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        13500, 1000, /* WB<->SWB */
        14000, 2000, /* SWB<->FB */
};
static const opus_int32 music_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        11000, 1000, /* WB<->SWB */
        12000, 2000, /* SWB<->FB */
};
/* Threshold bit-rates for switching between mono and stereo */
static const opus_int32 stereo_voice_threshold = 19000;
static const opus_int32 stereo_music_threshold = 17000;

/* Threshold bit-rate for switching between SILK/hybrid and CELT-only */
static const opus_int32 mode_thresholds[2][2] = {
      /* voice */ /* music */
      {  64000,      10000}, /* mono */
      {  44000,      10000}, /* stereo */
};

static const opus_int32 fec_thresholds[] = {
        12000, 1000, /* NB */
        14000, 1000, /* MB */
        16000, 1000, /* WB */
        20000, 1000, /* SWB */
        22000, 1000, /* FB */
};

/* maiopus: opus_encode() scratch. opus_encode_frame_native keeps the delayed
   PCM frame and the 2.5 ms prefill copy of stereo input, then runs SILK or
   CELT. */
#define OPUS_ENCODE_FRAME_SCRATCH_BYTES \
    (SCRATCH_BYTES((DELAY_COMPENSATION+VOICE_FRAME_SIZE)*2, opus_res)+SCRATCH_BYTES(2*VOICE_FS/400, opus_res))
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
       base_size = align(base_size - MAX_ENCODER_BUFFER*sizeof(opus_res));
    tot_size = base_size+silkEncSizeBytes+celtEncSizeBytes;
    if (st == NULL) {
        return tot_size;
    }
    OPUS_CLEAR((char*)st, tot_size);
    st->silk_enc_offset = base_size;
    st->celt_enc_offset = st->silk_enc_offset+silkEncSizeBytes;

    st->stream_channels = st->channels = channels;

    st->arch = opus_select_arch();

    silk_enc = (char*)st+st->silk_enc_offset;
    ret = silk_InitEncoder( silk_enc, st->channels, st->arch, &st->silk_mode );
    if(ret)return OPUS_INTERNAL_ERROR;

    /* default SILK parameters */
    st->silk_mode.nChannelsAPI              = channels;
    st->silk_mode.nChannelsInternal         = channels;
    st->silk_mode.API_sampleRate            = VOICE_FS;
    st->silk_mode.maxInternalSampleRate     = 16000;
    st->silk_mode.minInternalSampleRate     = 8000;
    st->silk_mode.desiredInternalSampleRate = 16000;
    st->silk_mode.payloadSize_ms            = 20;
    st->silk_mode.bitRate                   = 25000;
    st->silk_mode.packetLossPercentage      = 0;
    st->silk_mode.complexity                = 9;
    st->silk_mode.useInBandFEC              = 0;
    st->silk_mode.useDRED                   = 0;
    st->silk_mode.useDTX                    = 0;
    st->silk_mode.useCBR                    = 0;

    /* Create CELT encoder */
    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);
    err = celt_encoder_init(celt_enc, channels, st->arch);
    if(err!=OPUS_OK)return OPUS_INTERNAL_ERROR;
    celt_encoder_ctl(celt_enc, OPUS_SET_COMPLEXITY(st->silk_mode.complexity));

    /* Makes constrained VBR the default (safer for real-time use) */
    st->vbr_constraint = 1;
    st->user_bitrate_bps = OPUS_AUTO;
    st->bitrate_bps = 3000+VOICE_FS*channels;

    st->hybrid_stereo_width_Q14 = 1 << 14;
    st->prev_HB_gain = Q15ONE;
    st->variable_HP_smth2_Q15 = silk_LSHIFT( silk_lin2log( VARIABLE_HP_MIN_CUTOFF_HZ ), 8 );
    st->first = 1;
    st->mode = MODE_HYBRID;
    st->bandwidth = OPUS_BANDWIDTH_FULLBAND;

    return OPUS_OK;
}

static unsigned char gen_toc(int mode, int framerate, int bandwidth, int channels)
{
   int period;
   unsigned char toc;
   period = 0;
   while (framerate < 400)
   {
       framerate <<= 1;
       period++;
   }
   if (mode == MODE_SILK_ONLY)
   {
       toc = (bandwidth-OPUS_BANDWIDTH_NARROWBAND)<<5;
       toc |= (period-2)<<3;
   } else if (mode == MODE_CELT_ONLY)
   {
       int tmp = bandwidth-OPUS_BANDWIDTH_MEDIUMBAND;
       if (tmp < 0)
           tmp = 0;
       toc = 0x80;
       toc |= tmp << 5;
       toc |= period<<3;
   } else /* Hybrid */
   {
       toc = 0x60;
       toc |= (bandwidth-OPUS_BANDWIDTH_SUPERWIDEBAND)<<4;
       toc |= (period-2)<<3;
   }
   toc |= (channels==2)<<2;
   return toc;
}

static void hp_cutoff(const opus_res *in, opus_int32 cutoff_Hz, opus_res *out, opus_val32 *hp_mem, int len, int channels, int arch)
{
   opus_int32 B_Q28[ 3 ], A_Q28[ 2 ];
   opus_int32 Fc_Q19, r_Q28, r_Q22;
   (void)arch;

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
      silk_biquad_alt_stride2( in, B_Q28, A_Q28, hp_mem, out, len, arch );
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

static opus_int32 user_bitrate_to_bitrate(OpusEncoder *st, int max_data_bytes)
{
  opus_int32 max_bitrate, user_bitrate;
  max_bitrate = bits_to_bitrate(max_data_bytes*8, VOICE_FS, VOICE_FRAME_SIZE);
  if (st->user_bitrate_bps==OPUS_AUTO)
     user_bitrate = 60*VOICE_FRAME_RATE + VOICE_FS*st->channels;
  else if (st->user_bitrate_bps==OPUS_BITRATE_MAX)
     user_bitrate = 1500000;
  else
     user_bitrate = st->user_bitrate_bps;
  return IMIN(user_bitrate, max_bitrate);
}

static opus_val16 compute_stereo_width(const opus_res *pcm, StereoWidthState *mem)
{
   opus_val32 xx, xy, yy;
   opus_val16 sqrt_xx, sqrt_yy;
   opus_val16 qrrt_xx, qrrt_yy;
   int i;
   opus_val16 short_alpha;
   int shift = celt_ilog2(VOICE_FRAME_SIZE)-2;
   short_alpha = MULT16_16(25, Q15ONE)/VOICE_FRAME_RATE;
   xx=xy=yy=0;
   /* Unroll by 4 (the frame size is a multiple of 4). */
   for (i=0;i<VOICE_FRAME_SIZE-3;i+=4)
   {
      opus_val32 pxx=0;
      opus_val32 pxy=0;
      opus_val32 pyy=0;
      opus_val16 x, y;
      x = RES2VAL16(pcm[2*i]);
      y = RES2VAL16(pcm[2*i+1]);
      pxx = SHR32(MULT16_16(x,x),2);
      pxy = SHR32(MULT16_16(x,y),2);
      pyy = SHR32(MULT16_16(y,y),2);
      x = RES2VAL16(pcm[2*i+2]);
      y = RES2VAL16(pcm[2*i+3]);
      pxx += SHR32(MULT16_16(x,x),2);
      pxy += SHR32(MULT16_16(x,y),2);
      pyy += SHR32(MULT16_16(y,y),2);
      x = RES2VAL16(pcm[2*i+4]);
      y = RES2VAL16(pcm[2*i+5]);
      pxx += SHR32(MULT16_16(x,x),2);
      pxy += SHR32(MULT16_16(x,y),2);
      pyy += SHR32(MULT16_16(y,y),2);
      x = RES2VAL16(pcm[2*i+6]);
      y = RES2VAL16(pcm[2*i+7]);
      pxx += SHR32(MULT16_16(x,x),2);
      pxy += SHR32(MULT16_16(x,y),2);
      pyy += SHR32(MULT16_16(y,y),2);

      xx += SHR32(pxx, shift);
      xy += SHR32(pxy, shift);
      yy += SHR32(pyy, shift);
   }
   mem->XX += MULT16_32_Q15(short_alpha, xx-mem->XX);
   /* Rewritten to avoid overflows on abrupt sign change. */
   mem->XY = MULT16_32_Q15(Q15ONE - short_alpha, mem->XY) + MULT16_32_Q15(short_alpha, xy);
   mem->YY += MULT16_32_Q15(short_alpha, yy-mem->YY);
   mem->XX = MAX32(0, mem->XX);
   mem->XY = MAX32(0, mem->XY);
   mem->YY = MAX32(0, mem->YY);
   if (MAX32(mem->XX, mem->YY)>QCONST16(8e-4f, 18))
   {
      opus_val16 corr;
      opus_val16 ldiff;
      opus_val16 width;
      sqrt_xx = celt_sqrt(mem->XX);
      sqrt_yy = celt_sqrt(mem->YY);
      qrrt_xx = celt_sqrt(sqrt_xx);
      qrrt_yy = celt_sqrt(sqrt_yy);
      /* Inter-channel correlation */
      mem->XY = MIN32(mem->XY, sqrt_xx*sqrt_yy);
      corr = SHR32(frac_div32(mem->XY,EPSILON+MULT16_16(sqrt_xx,sqrt_yy)),16);
      /* Approximate loudness difference */
      ldiff = MULT16_16(Q15ONE, ABS16(qrrt_xx-qrrt_yy))/(EPSILON+qrrt_xx+qrrt_yy);
      width = MULT16_16_Q15(MIN16(Q15ONE, celt_sqrt(QCONST32(1.f,30)-MULT16_16(corr,corr))), ldiff);
      /* Smoothing over one second */
      mem->smoothed_width += (width-mem->smoothed_width)/VOICE_FRAME_RATE;
      /* Peak follower */
      mem->max_follower = MAX16(mem->max_follower-QCONST16(.02f,15)/VOICE_FRAME_RATE, mem->smoothed_width);
   }
   return EXTRACT16(MIN32(Q15ONE, MULT16_16(20, mem->max_follower)));
}

static int decide_fec(int useInBandFEC, int PacketLoss_perc, int last_fec, int mode, int *bandwidth, opus_int32 rate)
{
   int orig_bandwidth;
   if (!useInBandFEC || PacketLoss_perc == 0 || mode == MODE_CELT_ONLY)
      return 0;
   orig_bandwidth = *bandwidth;
   for (;;)
   {
      opus_int32 hysteresis;
      opus_int32 LBRR_rate_thres_bps;
      /* Compute threshold for using FEC at the current bandwidth setting */
      LBRR_rate_thres_bps = fec_thresholds[2*(*bandwidth - OPUS_BANDWIDTH_NARROWBAND)];
      hysteresis = fec_thresholds[2*(*bandwidth - OPUS_BANDWIDTH_NARROWBAND) + 1];
      if (last_fec == 1) LBRR_rate_thres_bps -= hysteresis;
      if (last_fec == 0) LBRR_rate_thres_bps += hysteresis;
      LBRR_rate_thres_bps = silk_SMULWB( silk_MUL( LBRR_rate_thres_bps,
            125 - silk_min( PacketLoss_perc, 25 ) ), SILK_FIX_CONST( 0.01, 16 ) );
      /* If loss <= 5%, we look at whether we have enough rate to enable FEC.
         If loss > 5%, we decrease the bandwidth until we can enable FEC. */
      if (rate > LBRR_rate_thres_bps)
         return 1;
      else if (PacketLoss_perc <= 5)
         return 0;
      else if (*bandwidth > OPUS_BANDWIDTH_NARROWBAND)
         (*bandwidth)--;
      else
         break;
   }
   /* Couldn't find any bandwidth to enable FEC, keep original bandwidth. */
   *bandwidth = orig_bandwidth;
   return 0;
}

static int compute_silk_rate_for_hybrid(int rate, int bandwidth, int fec, int channels) {
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
   if (bandwidth==OPUS_BANDWIDTH_SUPERWIDEBAND)
      silk_rate += 300;
   silk_rate *= channels;
   /* Small adjustment for stereo (calibrated for 32 kb/s, haven't tried other bitrates). */
   if (channels == 2 && rate >= 12000)
      silk_rate -= 1000;
   return silk_rate;
}

/* Returns the equivalent bitrate corresponding to 20 ms frames,
   complexity 10 VBR operation. */
static opus_int32 compute_equiv_rate(opus_int32 bitrate, int mode, int complexity, int loss)
{
   opus_int32 equiv;
   equiv = bitrate;
   /* Complexity makes about 10% difference (from 0 to 10) in general. */
   equiv = equiv * (90+complexity)/100;
   if (mode == MODE_SILK_ONLY || mode == MODE_HYBRID)
   {
      /* SILK complexity 0-1 uses the non-delayed-decision NSQ, which
         costs about 20%. */
      if (complexity<2)
         equiv = equiv*4/5;
      equiv -= equiv*loss/(6*loss + 10);
   } else if (mode == MODE_CELT_ONLY) {
      /* CELT complexity 0-4 doesn't have the pitch filter, which costs
         about 10%. */
      if (complexity<5)
         equiv = equiv*9/10;
   } else {
      /* Mode not known yet */
      /* Half the SILK loss*/
      equiv -= equiv*loss/(12*loss + 20);
   }
   return equiv;
}

static int is_digital_silence(const opus_res* pcm, int channels)
{
   return celt_maxabs_res(pcm, VOICE_FRAME_SIZE*channels) == 0;
}

static opus_val32 compute_frame_energy(const opus_res *pcm, int channels)
{
   int i;
   opus_val32 sample_max;
   int max_shift;
   int shift;
   opus_val32 energy = 0;
   int len = VOICE_FRAME_SIZE*channels;
   /* Max amplitude in the signal */
   sample_max = RES2INT16(celt_maxabs_res(pcm, len));

   /* Compute the right shift required in the MAC to avoid an overflow */
   max_shift = celt_ilog2(len);
   shift = IMAX(0, (celt_ilog2(1+sample_max) << 1) + max_shift - 28);

   /* Compute the energy */
   for (i=0; i<len; i++)
      energy += SHR32(MULT16_16(RES2INT16(pcm[i]), RES2INT16(pcm[i])), shift);

   /* Normalize energy by the frame size and left-shift back to the original position */
   energy /= len;
   energy = SHL32(energy, shift);

   return energy;
}

/* Decides if DTX should be turned on (=1) or off (=0) after one more 20 ms frame */
static int decide_dtx_mode(opus_int activity,            /* indicates if this frame contains speech/music */
                           int *nb_no_activity_ms_Q1     /* number of consecutive milliseconds with no activity, in Q1 */
                           )

{
   if (!activity)
   {
      /* The number of consecutive DTX frames should be within the allowed bounds. */
      (*nb_no_activity_ms_Q1) += 2*20;
      if (*nb_no_activity_ms_Q1 > NB_SPEECH_FRAMES_BEFORE_DTX*20*2)
      {
         if (*nb_no_activity_ms_Q1 <= (NB_SPEECH_FRAMES_BEFORE_DTX + MAX_CONSECUTIVE_DTX)*20*2)
            /* Valid frame for DTX! */
            return 1;
         else
            (*nb_no_activity_ms_Q1) = NB_SPEECH_FRAMES_BEFORE_DTX*20*2;
      }
   } else
      (*nb_no_activity_ms_Q1) = 0;

   return 0;
}

static int compute_redundancy_bytes(opus_int32 max_data_bytes, opus_int32 bitrate_bps, int channels)
{
   int redundancy_bytes_cap;
   int redundancy_bytes;
   opus_int32 redundancy_rate;
   int base_bits;
   opus_int32 available_bits;
   base_bits = (40*channels+20);

   /* Equivalent rate for 5 ms frames. */
   redundancy_rate = bitrate_bps + base_bits*(200 - VOICE_FRAME_RATE);
   /* For VBR, further increase the bitrate if we can afford it. It's pretty short
      and we'll avoid artefacts. */
   redundancy_rate = 3*redundancy_rate/2;
   redundancy_bytes = redundancy_rate/1600;

   /* Compute the max rate we can use given VBR with cap. */
   available_bits = max_data_bytes*8 - 2*base_bits;
   redundancy_bytes_cap = (available_bits*240/(240+VOICE_FRAME_SIZE) + base_bits)/8;
   redundancy_bytes = IMIN(redundancy_bytes, redundancy_bytes_cap);
   /* It we can't get enough bits for redundancy to be worth it, rely on the decoder PLC. */
   if (redundancy_bytes > 4 + 8*channels)
      redundancy_bytes = IMIN(257, redundancy_bytes);
   else
      redundancy_bytes = 0;
   return redundancy_bytes;
}

static opus_int32 opus_encode_frame_native(OpusEncoder *st, const opus_res *pcm,
                unsigned char *data, opus_int32 orig_max_data_bytes,
                int is_silence, int redundancy, int celt_to_silk, int prefill,
                opus_int32 equiv_rate, int to_celt, char *scratch)
{
    void *silk_enc;
    CELTEncoder *celt_enc;
    const CELTMode *celt_mode=NULL;
    int ret=0;
    int max_data_bytes;
    opus_int32 nBytes;
    ec_enc enc;
    int bits_target;
    int start_band = 0;
    int redundancy_bytes = 0; /* Number of bytes to use for redundancy frame */
    int nb_compr_bytes;
    opus_uint32 redundant_rng = 0;
    int cutoff_Hz;
    int hp_freq_smth1;
    opus_val16 HB_gain;
    int curr_bandwidth;
    opus_int activity = VAD_NO_DECISION;
    VARDECL(opus_res, pcm_buf);
    VARDECL(opus_res, tmp_prefill);
    SAVE_STACK;

    max_data_bytes = IMIN(orig_max_data_bytes, 1276);
    st->rangeFinal = 0;
    silk_enc = (char*)st+st->silk_enc_offset;
    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);
    celt_encoder_ctl(celt_enc, CELT_GET_MODE(&celt_mode));
    curr_bandwidth = st->bandwidth;

    if (is_silence)
    {
       activity = 0;
    }
    else if (st->mode == MODE_CELT_ONLY) {
       opus_val32 noise_energy = compute_frame_energy(pcm, st->channels);
       /* Boosting peak energy a bit because we didn't just average the active frames. */
       activity = st->peak_signal_energy < (QCONST16(PSEUDO_SNR_THRESHOLD, 0) * (opus_val64)HALF32(noise_energy));
    }

    /* For the first frame at a new SILK bandwidth */
    if (st->silk_bw_switch)
    {
       redundancy = 1;
       celt_to_silk = 1;
       st->silk_bw_switch = 0;
       /* Do a prefill without resetting the sampling rate control. */
       prefill=2;
    }

    /* If we decided to go with CELT, make sure redundancy is off, no matter what
       we decided earlier. */
    if (st->mode == MODE_CELT_ONLY)
        redundancy = 0;

    if (redundancy)
    {
       redundancy_bytes = compute_redundancy_bytes(max_data_bytes, st->bitrate_bps, st->stream_channels);
       if (redundancy_bytes == 0)
          redundancy = 0;
    }

    bits_target = IMIN(8*(max_data_bytes-redundancy_bytes), bitrate_to_bits(st->bitrate_bps, VOICE_FS, VOICE_FRAME_SIZE)) - 8;

    data += 1;

    ec_enc_init(&enc, data, orig_max_data_bytes-1);

    ALLOC(pcm_buf, (DELAY_COMPENSATION+VOICE_FRAME_SIZE)*st->channels, opus_res);
    OPUS_COPY(pcm_buf, &st->delay_buffer[(MAX_ENCODER_BUFFER-DELAY_COMPENSATION)*st->channels], DELAY_COMPENSATION*st->channels);

    if (st->mode == MODE_CELT_ONLY)
       hp_freq_smth1 = silk_LSHIFT( silk_lin2log( VARIABLE_HP_MIN_CUTOFF_HZ ), 8 );
    else
       hp_freq_smth1 = ((silk_encoder*)silk_enc)->state_Fxx[0].sCmn.variable_HP_smth1_Q15;

    st->variable_HP_smth2_Q15 = silk_SMLAWB( st->variable_HP_smth2_Q15,
          hp_freq_smth1 - st->variable_HP_smth2_Q15, SILK_FIX_CONST( VARIABLE_HP_SMTH_COEF2, 16 ) );

    /* convert from log scale to Hertz */
    cutoff_Hz = silk_log2lin( silk_RSHIFT( st->variable_HP_smth2_Q15, 8 ) );

    hp_cutoff(pcm, cutoff_Hz, &pcm_buf[DELAY_COMPENSATION*st->channels], st->hp_mem, VOICE_FRAME_SIZE, st->channels, st->arch);

    /* SILK processing */
    HB_gain = Q15ONE;
    if (st->mode != MODE_CELT_ONLY)
    {
        opus_int32 total_bitRate, celt_rate;
        const opus_res *pcm_silk;

        /* Distribute bits between SILK and CELT */
        total_bitRate = bits_to_bitrate(bits_target, VOICE_FS, VOICE_FRAME_SIZE);
        if( st->mode == MODE_HYBRID ) {
            /* Base rate for SILK */
            st->silk_mode.bitRate = compute_silk_rate_for_hybrid(total_bitRate,
                  curr_bandwidth, st->silk_mode.LBRR_coded, st->stream_channels);
            /* Increasingly attenuate high band when it gets allocated fewer bits */
            celt_rate = total_bitRate - st->silk_mode.bitRate;
            HB_gain = Q15ONE - SHR32(celt_exp2(-celt_rate * QCONST16(1.f/1024, 10)), 1);
        } else {
            /* SILK gets all bits */
            st->silk_mode.bitRate = total_bitRate;
        }

        st->silk_mode.payloadSize_ms = 20;
        st->silk_mode.nChannelsAPI = st->channels;
        st->silk_mode.nChannelsInternal = st->stream_channels;
        if (curr_bandwidth == OPUS_BANDWIDTH_NARROWBAND) {
            st->silk_mode.desiredInternalSampleRate = 8000;
        } else if (curr_bandwidth == OPUS_BANDWIDTH_MEDIUMBAND) {
            st->silk_mode.desiredInternalSampleRate = 12000;
        } else {
            celt_assert( st->mode == MODE_HYBRID || curr_bandwidth == OPUS_BANDWIDTH_WIDEBAND );
            st->silk_mode.desiredInternalSampleRate = 16000;
        }
        if( st->mode == MODE_HYBRID ) {
            /* Don't allow bandwidth reduction at lowest bitrates in hybrid mode */
            st->silk_mode.minInternalSampleRate = 16000;
        } else {
            st->silk_mode.minInternalSampleRate = 8000;
        }

        st->silk_mode.maxInternalSampleRate = 16000;
        if (st->mode == MODE_SILK_ONLY)
        {
           opus_int32 effective_max_rate = bits_to_bitrate(max_data_bytes*8, VOICE_FS, VOICE_FRAME_SIZE);
           if (effective_max_rate < 8000)
           {
              st->silk_mode.maxInternalSampleRate = 12000;
              st->silk_mode.desiredInternalSampleRate = IMIN(12000, st->silk_mode.desiredInternalSampleRate);
           }
           if (effective_max_rate < 7000)
           {
              st->silk_mode.maxInternalSampleRate = 8000;
              st->silk_mode.desiredInternalSampleRate = IMIN(8000, st->silk_mode.desiredInternalSampleRate);
           }
        }

        /* Call SILK encoder for the low band */

        /* Max bits for SILK, counting ToC, redundancy bytes, and optionally redundancy. */
        st->silk_mode.maxBits = (max_data_bytes-1)*8;
        if (redundancy && redundancy_bytes >= 2)
        {
           /* Counting 1 bit for redundancy position and 20 bits for flag+size (only for hybrid). */
           st->silk_mode.maxBits -= redundancy_bytes*8 + 1;
           if (st->mode == MODE_HYBRID)
              st->silk_mode.maxBits -= 20;
        }
        /* Constrained VBR. */
        if (st->mode == MODE_HYBRID)
        {
           /* Compute SILK bitrate corresponding to the max total bits available */
           opus_int32 maxBitRate = compute_silk_rate_for_hybrid(st->silk_mode.maxBits*VOICE_FRAME_RATE,
                 curr_bandwidth, st->silk_mode.LBRR_coded, st->stream_channels);
           st->silk_mode.maxBits = bitrate_to_bits(maxBitRate, VOICE_FS, VOICE_FRAME_SIZE);
        }

        if (prefill)
        {
            opus_int32 zero=0;
            int prefill_offset;
            /* Use a smooth onset for the SILK prefill to avoid the encoder trying to encode
               a discontinuity. The exact location is what we need to avoid leaving any "gap"
               in the audio when mixing with the redundant CELT frame. Here we can afford to
               overwrite st->delay_buffer because the only thing that uses it before it gets
               rewritten is tmp_prefill[] and even then only the part after the ramp really
               gets used (rather than sent to the encoder and discarded) */
            prefill_offset = st->channels*(MAX_ENCODER_BUFFER-DELAY_COMPENSATION-VOICE_FS/400);
            gain_fade(st->delay_buffer+prefill_offset, st->delay_buffer+prefill_offset,
                  0, Q15ONE, celt_mode->overlap, VOICE_FS/400, st->channels, celt_mode->window);
            OPUS_CLEAR(st->delay_buffer, prefill_offset);
            pcm_silk = st->delay_buffer;
            silk_Encode( silk_enc, &st->silk_mode, pcm_silk, MAX_ENCODER_BUFFER, NULL, &zero, prefill, activity, scratch );
            /* Prevent a second switch in the real encode call. */
            st->silk_mode.opusCanSwitch = 0;
        }

        pcm_silk = pcm_buf+DELAY_COMPENSATION*st->channels;
        ret = silk_Encode( silk_enc, &st->silk_mode, pcm_silk, VOICE_FRAME_SIZE, &enc, &nBytes, 0, activity, scratch );
        if( ret ) {
           RESTORE_STACK;
           return OPUS_INTERNAL_ERROR;
        }

        /* Extract SILK internal bandwidth for signaling in first byte */
        if( st->mode == MODE_SILK_ONLY ) {
            if( st->silk_mode.internalSampleRate == 8000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_NARROWBAND;
            } else if( st->silk_mode.internalSampleRate == 12000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
            } else if( st->silk_mode.internalSampleRate == 16000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_WIDEBAND;
            }
        } else {
            celt_assert( st->silk_mode.internalSampleRate == 16000 );
        }

        st->silk_mode.opusCanSwitch = st->silk_mode.switchReady;

        if (activity == VAD_NO_DECISION) {
           activity = (st->silk_mode.signalType != TYPE_NO_VOICE_ACTIVITY);
        }
        if (nBytes==0)
        {
           st->rangeFinal = 0;
           data[-1] = gen_toc(st->mode, VOICE_FRAME_RATE, curr_bandwidth, st->stream_channels);
           RESTORE_STACK;
           return 1;
        }

        if (st->silk_mode.opusCanSwitch)
        {
           redundancy_bytes = compute_redundancy_bytes(max_data_bytes, st->bitrate_bps, st->stream_channels);
           redundancy = (redundancy_bytes != 0);
           celt_to_silk = 0;
           st->silk_bw_switch = 1;
        }
    }

    /* CELT processing */
    {
        int endband=21;

        switch(curr_bandwidth)
        {
            case OPUS_BANDWIDTH_NARROWBAND:
                endband = 13;
                break;
            case OPUS_BANDWIDTH_MEDIUMBAND:
            case OPUS_BANDWIDTH_WIDEBAND:
                endband = 17;
                break;
            case OPUS_BANDWIDTH_SUPERWIDEBAND:
                endband = 19;
                break;
            case OPUS_BANDWIDTH_FULLBAND:
                endband = 21;
                break;
        }
        celt_encoder_ctl(celt_enc, CELT_SET_END_BAND(endband));
        celt_encoder_ctl(celt_enc, CELT_SET_CHANNELS(st->stream_channels));
        celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
    }
    if (st->mode != MODE_SILK_ONLY)
    {
        /* We may still decide to disable prediction later */
        celt_encoder_ctl(celt_enc, CELT_SET_PREDICTION(2));
    }

    ALLOC(tmp_prefill, st->channels*VOICE_FS/400, opus_res);
    if (st->mode != MODE_SILK_ONLY && st->mode != st->prev_mode && st->prev_mode > 0)
    {
       OPUS_COPY(tmp_prefill, &st->delay_buffer[(MAX_ENCODER_BUFFER-DELAY_COMPENSATION-VOICE_FS/400)*st->channels], st->channels*VOICE_FS/400);
    }

    /* The frame plus the delay compensation always exceeds the buffer. */
    OPUS_COPY(st->delay_buffer, &pcm_buf[(VOICE_FRAME_SIZE+DELAY_COMPENSATION-MAX_ENCODER_BUFFER)*st->channels], MAX_ENCODER_BUFFER*st->channels);
    /* gain_fade() and stereo_fade() need to be after the buffer copying
       because we don't want any of this to affect the SILK part */
    if( st->prev_HB_gain < Q15ONE || HB_gain < Q15ONE ) {
       gain_fade(pcm_buf, pcm_buf,
             st->prev_HB_gain, HB_gain, celt_mode->overlap, VOICE_FRAME_SIZE, st->channels, celt_mode->window);
    }
    st->prev_HB_gain = HB_gain;
    if (st->mode != MODE_HYBRID || st->stream_channels==1)
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

    if ( st->mode != MODE_CELT_ONLY && ec_tell(&enc)+17+20*(st->mode == MODE_HYBRID) <= 8*(max_data_bytes-1))
    {
        /* For SILK mode, the redundancy is inferred from the length */
        if (st->mode == MODE_HYBRID)
           ec_enc_bit_logp(&enc, redundancy, 12);
        if (redundancy)
        {
            int max_redundancy;
            ec_enc_bit_logp(&enc, celt_to_silk, 1);
            if (st->mode == MODE_HYBRID)
            {
               /* Reserve the 8 bits needed for the redundancy length,
                  and at least a few bits for CELT if possible */
               max_redundancy = (max_data_bytes-1)-((ec_tell(&enc)+8+3+7)>>3);
            }
            else
               max_redundancy = (max_data_bytes-1)-((ec_tell(&enc)+7)>>3);
            /* Target the same bit-rate for redundancy as for the rest,
               up to a max of 257 bytes */
            redundancy_bytes = IMIN(max_redundancy, redundancy_bytes);
            redundancy_bytes = IMIN(257, IMAX(2, redundancy_bytes));
            if (st->mode == MODE_HYBRID)
                ec_enc_uint(&enc, redundancy_bytes-2, 256);
        }
    } else {
        redundancy = 0;
    }

    if (!redundancy)
    {
       st->silk_bw_switch = 0;
       redundancy_bytes = 0;
    }
    if (st->mode != MODE_CELT_ONLY)start_band=17;

    if (st->mode == MODE_SILK_ONLY)
    {
        ret = (ec_tell(&enc)+7)>>3;
        ec_enc_done(&enc);
        nb_compr_bytes = ret;
    } else {
        nb_compr_bytes = (max_data_bytes-1)-redundancy_bytes;
        ec_enc_shrink(&enc, nb_compr_bytes);
    }

    if (st->mode == MODE_HYBRID) {
       SILKInfo info;
       info.signalType = st->silk_mode.signalType;
       info.offset = st->silk_mode.offset;
       celt_encoder_ctl(celt_enc, CELT_SET_SILK_INFO(&info));
    }

    /* 5 ms redundant frame for CELT->SILK */
    if (redundancy && celt_to_silk)
    {
        int err;
        celt_encoder_ctl(celt_enc, CELT_SET_START_BAND(0));
        celt_encoder_ctl(celt_enc, OPUS_SET_VBR(0));
        celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
        err = celt_encode_with_ec(celt_enc, pcm_buf, VOICE_FS/200, data+nb_compr_bytes, redundancy_bytes, NULL, scratch);
        if (err < 0)
        {
           RESTORE_STACK;
           return OPUS_INTERNAL_ERROR;
        }
        celt_encoder_ctl(celt_enc, OPUS_GET_FINAL_RANGE(&redundant_rng));
        celt_encoder_ctl(celt_enc, OPUS_RESET_STATE);
    }

    celt_encoder_ctl(celt_enc, CELT_SET_START_BAND(start_band));

    data[-1] = 0;
    if (st->mode != MODE_SILK_ONLY)
    {
        celt_encoder_ctl(celt_enc, OPUS_SET_VBR(1));
        if (st->mode == MODE_HYBRID)
        {
            celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(st->bitrate_bps-st->silk_mode.bitRate));
            celt_encoder_ctl(celt_enc, OPUS_SET_VBR_CONSTRAINT(0));
        } else {
            celt_encoder_ctl(celt_enc, OPUS_SET_VBR_CONSTRAINT(st->vbr_constraint));
            celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(st->bitrate_bps));
        }
        if (st->mode != st->prev_mode && st->prev_mode > 0)
        {
           unsigned char dummy[2];
           celt_encoder_ctl(celt_enc, OPUS_RESET_STATE);

           /* Prefilling */
           celt_encode_with_ec(celt_enc, tmp_prefill, VOICE_FS/400, dummy, 2, NULL, scratch);
           celt_encoder_ctl(celt_enc, CELT_SET_PREDICTION(0));
        }
        /* If false, we already busted the budget and we'll end up with a "PLC frame" */
        if (ec_tell(&enc) <= 8*nb_compr_bytes)
        {
           ret = celt_encode_with_ec(celt_enc, pcm_buf, VOICE_FRAME_SIZE, NULL, nb_compr_bytes, &enc, scratch);
           if (ret < 0)
           {
              RESTORE_STACK;
              return OPUS_INTERNAL_ERROR;
           }
           /* Put CELT->SILK redundancy data in the right place. */
           if (redundancy && celt_to_silk && st->mode==MODE_HYBRID && nb_compr_bytes != ret)
           {
              OPUS_MOVE(data+ret, data+nb_compr_bytes, redundancy_bytes);
              nb_compr_bytes = ret+redundancy_bytes;
           }
        }
        celt_encoder_ctl(celt_enc, OPUS_GET_FINAL_RANGE(&st->rangeFinal));
    } else {
       st->rangeFinal = enc.rng;
    }

    /* 5 ms redundant frame for SILK->CELT */
    if (redundancy && !celt_to_silk)
    {
        int err;
        unsigned char dummy[2];
        int N2, N4;
        N2 = VOICE_FS/200;
        N4 = VOICE_FS/400;

        celt_encoder_ctl(celt_enc, OPUS_RESET_STATE);
        celt_encoder_ctl(celt_enc, CELT_SET_START_BAND(0));
        celt_encoder_ctl(celt_enc, CELT_SET_PREDICTION(0));
        celt_encoder_ctl(celt_enc, OPUS_SET_VBR(0));
        celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));

        if (st->mode == MODE_HYBRID)
        {
           /* Shrink packet to what the encoder actually used. */
           nb_compr_bytes = ret;
           ec_enc_shrink(&enc, nb_compr_bytes);
        }
        /* NOTE: We could speed this up slightly (at the expense of code size) by just adding a function that prefills the buffer */
        celt_encode_with_ec(celt_enc, pcm_buf+st->channels*(VOICE_FRAME_SIZE-N2-N4), N4, dummy, 2, NULL, scratch);

        err = celt_encode_with_ec(celt_enc, pcm_buf+st->channels*(VOICE_FRAME_SIZE-N2), N2, data+nb_compr_bytes, redundancy_bytes, NULL, scratch);
        if (err < 0)
        {
           RESTORE_STACK;
           return OPUS_INTERNAL_ERROR;
        }
        celt_encoder_ctl(celt_enc, OPUS_GET_FINAL_RANGE(&redundant_rng));
    }


    /* Signalling the mode in the first byte */
    data--;
    data[0] |= gen_toc(st->mode, VOICE_FRAME_RATE, curr_bandwidth, st->stream_channels);

    st->rangeFinal ^= redundant_rng;

    if (to_celt)
        st->prev_mode = MODE_CELT_ONLY;
    else
        st->prev_mode = st->mode;
    st->prev_channels = st->stream_channels;

    st->first = 0;

    /* DTX decision */
    if (st->use_dtx && !st->silk_mode.useDTX)
    {
       if (decide_dtx_mode(activity, &st->nb_no_activity_ms_Q1))
       {
          st->rangeFinal = 0;
          data[0] = gen_toc(st->mode, VOICE_FRAME_RATE, curr_bandwidth, st->stream_channels);
          RESTORE_STACK;
          return 1;
       }
    } else {
       st->nb_no_activity_ms_Q1 = 0;
    }

    /* In the unlikely case that the SILK encoder busted its target, tell
       the decoder to call the PLC */
    if (ec_tell(&enc) > (max_data_bytes-1)*8)
    {
       if (max_data_bytes < 2)
       {
          RESTORE_STACK;
          return OPUS_BUFFER_TOO_SMALL;
       }
       data[1] = 0;
       ret = 1;
       st->rangeFinal = 0;
    } else if (st->mode==MODE_SILK_ONLY&&!redundancy)
    {
       /*When in LPC only mode it's perfectly
         reasonable to strip off trailing zero bytes as
         the required range decoder behavior is to
         fill these in. This can't be done when the MDCT
         modes are used because the decoder needs to know
         the actual length for allocation purposes.*/
       while(ret>2&&data[ret]==0)ret--;
    }
    /* Count ToC and redundancy */
    ret += 1+redundancy_bytes;
    RESTORE_STACK;
    return ret;
}

static opus_int32 opus_encode_native(OpusEncoder *st, const opus_res *pcm, int frame_size,
                unsigned char *data, opus_int32 out_data_bytes, char *scratch)
{
    void *silk_enc;
    int i;
    int prefill=0;
    int redundancy = 0;
    int celt_to_silk = 0;
    int to_celt = 0;
    opus_int32 equiv_rate;
    opus_int32 max_rate; /* Max bitrate we're allowed to use */
    int curr_bandwidth;
    opus_int32 max_data_bytes; /* Max number of bytes we're allowed to use */
    opus_val16 stereo_width;
    int is_silence;

    /* Just avoid insane packet sizes here, but the real bounds are applied later on. */
    max_data_bytes = IMIN(1276*6, out_data_bytes);

    st->rangeFinal = 0;
    if (frame_size != VOICE_FRAME_SIZE || max_data_bytes <= 0)
       return OPUS_BAD_ARG;

    silk_enc = (char*)st+st->silk_enc_offset;

    is_silence = is_digital_silence(pcm, st->channels);

    /* Track the peak signal energy */
    if (!is_silence)
    {
       st->peak_signal_energy = MAX32(MULT16_32_Q15(QCONST16(0.999f, 15), st->peak_signal_energy),
             compute_frame_energy(pcm, st->channels));
    }
    if (st->channels==2)
       stereo_width = compute_stereo_width(pcm, &st->width_mem);
    else
       stereo_width = 0;
    st->bitrate_bps = user_bitrate_to_bitrate(st, max_data_bytes);

    if (max_data_bytes<3 || st->bitrate_bps < 3*VOICE_FRAME_RATE*8)
    {
       /*If the space is too low to do something useful, emit 'PLC' frames.*/
       int tocmode = st->mode;
       int bw = st->bandwidth;

       if(tocmode==MODE_SILK_ONLY&&bw>OPUS_BANDWIDTH_WIDEBAND)
          bw=OPUS_BANDWIDTH_WIDEBAND;
       else if (tocmode==MODE_CELT_ONLY&&bw==OPUS_BANDWIDTH_MEDIUMBAND)
          bw=OPUS_BANDWIDTH_NARROWBAND;
       else if (tocmode==MODE_HYBRID&&bw<=OPUS_BANDWIDTH_SUPERWIDEBAND)
          bw=OPUS_BANDWIDTH_SUPERWIDEBAND;

       data[0] = gen_toc(tocmode, VOICE_FRAME_RATE, bw, st->stream_channels);
       return 1;
    }
    max_rate = bits_to_bitrate(max_data_bytes*8, VOICE_FS, VOICE_FRAME_SIZE);

    /* Equivalent 20-ms rate for mode/channel/bandwidth decisions */
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

    /* Allow SILK DTX if DTX is enabled but the generalized DTX cannot be used,
       e.g. because of the complexity setting or sample rate. */
    st->silk_mode.useDTX = st->use_dtx && !is_silence;

    /* Mode selection */
    {
       opus_int32 mode_voice, mode_music;
       opus_int32 threshold;

       /* Interpolate based on stereo width */
       mode_voice = (opus_int32)(MULT16_32_Q15(Q15ONE-stereo_width,mode_thresholds[0][0])
             + MULT16_32_Q15(stereo_width,mode_thresholds[1][0]));
       mode_music = (opus_int32)(MULT16_32_Q15(Q15ONE-stereo_width,mode_thresholds[1][1])
             + MULT16_32_Q15(stereo_width,mode_thresholds[1][1]));
       /* Interpolate based on speech/music probability */
       threshold = mode_music + ((VOICE_ESTIMATE*VOICE_ESTIMATE*(mode_voice-mode_music))>>14);
       /* Bias towards SILK for VoIP because of some useful features */
       threshold += 8000;

       /* Hysteresis */
       if (st->prev_mode == MODE_CELT_ONLY)
           threshold -= 4000;
       else if (st->prev_mode>0)
           threshold += 4000;

       st->mode = (equiv_rate >= threshold) ? MODE_CELT_ONLY: MODE_SILK_ONLY;

       /* When FEC is enabled and there's enough packet loss, use SILK. */
       if (st->silk_mode.useInBandFEC && st->silk_mode.packetLossPercentage > (128-VOICE_ESTIMATE)>>4)
          st->mode = MODE_SILK_ONLY;
       /* When encoding voice and DTX is enabled but the generalized DTX cannot be used,
          use SILK in order to make use of its DTX. */
       if (st->silk_mode.useDTX)
          st->mode = MODE_SILK_ONLY;

       /* If max_data_bytes represents less than 6 kb/s, switch to CELT-only mode */
       if (max_data_bytes < bitrate_to_bits(6000, VOICE_FS, VOICE_FRAME_SIZE)/8)
          st->mode = MODE_CELT_ONLY;
    }

    if (st->prev_mode > 0 &&
        ((st->mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) ||
    (st->mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY)))
    {
        redundancy = 1;
        celt_to_silk = (st->mode != MODE_CELT_ONLY);
        if (!celt_to_silk)
        {
            /* Stay in SILK/hybrid for this frame and switch to CELT after it. */
            st->mode = st->prev_mode;
            to_celt = 1;
        }
    }

    if (st->stream_channels == 1 && st->prev_channels ==2 && st->silk_mode.toMono==0
          && st->mode != MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY)
    {
       /* Delay stereo->mono transition by two frames so that SILK can do a smooth downmix */
       st->silk_mode.toMono = 1;
       st->stream_channels = 2;
    } else {
       st->silk_mode.toMono = 0;
    }

    /* Update equivalent rate with mode decision. */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, st->mode, st->silk_mode.complexity, st->silk_mode.packetLossPercentage);

    if (st->mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY)
    {
        silk_EncControlStruct dummy;
        silk_InitEncoder( silk_enc, st->channels, st->arch, &dummy);
        prefill=1;
    }

    /* Automatic (rate-dependent) bandwidth selection */
    if (st->mode == MODE_CELT_ONLY || st->first || st->silk_mode.allowBandwidthSwitch)
    {
        opus_int32 bandwidth_thresholds[8];
        int bandwidth = OPUS_BANDWIDTH_FULLBAND;

        /* Interpolate bandwidth thresholds depending on voice estimation */
        for (i=0;i<8;i++)
        {
           bandwidth_thresholds[i] = music_bandwidth_thresholds[i]
                    + ((VOICE_ESTIMATE*VOICE_ESTIMATE*(voice_bandwidth_thresholds[i]-music_bandwidth_thresholds[i]))>>14);
        }
        do {
            int threshold, hysteresis;
            threshold = bandwidth_thresholds[2*(bandwidth-OPUS_BANDWIDTH_MEDIUMBAND)];
            hysteresis = bandwidth_thresholds[2*(bandwidth-OPUS_BANDWIDTH_MEDIUMBAND)+1];
            if (!st->first)
            {
                if (st->auto_bandwidth >= bandwidth)
                    threshold -= hysteresis;
                else
                    threshold += hysteresis;
            }
            if (equiv_rate >= threshold)
                break;
        } while (--bandwidth>OPUS_BANDWIDTH_NARROWBAND);
        /* We don't use mediumband anymore, except during mode transitions. */
        if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
           bandwidth = OPUS_BANDWIDTH_WIDEBAND;
        st->bandwidth = st->auto_bandwidth = bandwidth;
        /* Prevents any transition to SWB/FB until the SILK layer has fully
           switched to WB mode and turned the variable LP filter off */
        if (!st->first && st->mode != MODE_CELT_ONLY && !st->silk_mode.inWBmodeWithoutVariableLP && st->bandwidth > OPUS_BANDWIDTH_WIDEBAND)
            st->bandwidth = OPUS_BANDWIDTH_WIDEBAND;
    }

    /* This prevents us from using hybrid at unsafe max rates */
    if (st->mode != MODE_CELT_ONLY && max_rate < 15000)
    {
       st->bandwidth = IMIN(st->bandwidth, OPUS_BANDWIDTH_WIDEBAND);
    }

    st->silk_mode.LBRR_coded = decide_fec(st->silk_mode.useInBandFEC, st->silk_mode.packetLossPercentage,
          st->silk_mode.LBRR_coded, st->mode, &st->bandwidth, equiv_rate);

    /* CELT mode doesn't support mediumband, use wideband instead */
    if (st->mode == MODE_CELT_ONLY && st->bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
        st->bandwidth = OPUS_BANDWIDTH_WIDEBAND;

    curr_bandwidth = st->bandwidth;

    /* Chooses the appropriate mode for speech
       *NEVER* switch to/from CELT-only mode here as this will invalidate some assumptions */
    if (st->mode == MODE_SILK_ONLY && curr_bandwidth > OPUS_BANDWIDTH_WIDEBAND)
        st->mode = MODE_HYBRID;
    if (st->mode == MODE_HYBRID && curr_bandwidth <= OPUS_BANDWIDTH_WIDEBAND)
        st->mode = MODE_SILK_ONLY;

    return opus_encode_frame_native(st, pcm, data, max_data_bytes,
          is_silence, redundancy, celt_to_silk, prefill, equiv_rate, to_celt, scratch);
}

opus_int32 opus_encode(OpusEncoder *st, const opus_int16 *pcm, int analysis_frame_size,
                unsigned char *data, opus_int32 max_data_bytes, void *scratch)
{
   return opus_encode_native(st, pcm, analysis_frame_size, data, max_data_bytes, (char*)scratch);
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
            if (value != OPUS_AUTO && value != OPUS_BITRATE_MAX)
            {
                if (value <= 0)
                    goto bad_arg;
                else if (value <= 500)
                    value = 500;
                else if (value > (opus_int32)750000*st->channels)
                    value = (opus_int32)750000*st->channels;
            }
            st->user_bitrate_bps = value;
        }
        break;
        case OPUS_GET_BITRATE_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = user_bitrate_to_bitrate(st, 1276);
        }
        break;
        case OPUS_SET_DTX_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<0 || value>1)
            {
               goto bad_arg;
            }
            st->use_dtx = value;
        }
        break;
        case OPUS_GET_DTX_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->use_dtx;
        }
        break;
        case OPUS_SET_COMPLEXITY_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<0 || value>10)
            {
               goto bad_arg;
            }
            st->silk_mode.complexity = value;
            celt_encoder_ctl(celt_enc, OPUS_SET_COMPLEXITY(value));
        }
        break;
        case OPUS_GET_COMPLEXITY_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->silk_mode.complexity;
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
        case OPUS_GET_INBAND_FEC_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->silk_mode.useInBandFEC;
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
        case OPUS_GET_PACKET_LOSS_PERC_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->silk_mode.packetLossPercentage;
        }
        break;
        case OPUS_SET_VBR_CONSTRAINT_REQUEST:
        {
            opus_int32 value = va_arg(ap, opus_int32);
            if(value<0 || value>1)
            {
               goto bad_arg;
            }
            st->vbr_constraint = value;
        }
        break;
        case OPUS_GET_VBR_CONSTRAINT_REQUEST:
        {
            opus_int32 *value = va_arg(ap, opus_int32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->vbr_constraint;
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
        case OPUS_GET_FINAL_RANGE_REQUEST:
        {
            opus_uint32 *value = va_arg(ap, opus_uint32*);
            if (!value)
            {
               goto bad_arg;
            }
            *value = st->rangeFinal;
        }
        break;
        case OPUS_RESET_STATE:
        {
           void *silk_enc;
           silk_EncControlStruct dummy;
           char *start;
           silk_enc = (char*)st+st->silk_enc_offset;

           start = (char*)&st->OPUS_ENCODER_RESET_START;
           OPUS_CLEAR(start, st->silk_enc_offset - (start - (char*)st));

           celt_encoder_ctl(celt_enc, OPUS_RESET_STATE);
           silk_InitEncoder( silk_enc, st->channels, st->arch, &dummy );
           st->stream_channels = st->channels;
           st->hybrid_stereo_width_Q14 = 1 << 14;
           st->prev_HB_gain = Q15ONE;
           st->first = 1;
           st->mode = MODE_HYBRID;
           st->bandwidth = OPUS_BANDWIDTH_FULLBAND;
           st->variable_HP_smth2_Q15 = silk_LSHIFT( silk_lin2log( VARIABLE_HP_MIN_CUTOFF_HZ ), 8 );
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
