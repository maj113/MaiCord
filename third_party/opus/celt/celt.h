/* Copyright (c) 2007-2008 CSIRO
   Copyright (c) 2007-2009 Xiph.Org Foundation
   Copyright (c) 2008 Gregory Maxwell
   Written by Jean-Marc Valin and Gregory Maxwell */
/**
  @file celt.h
  @brief Contains all the functions for encoding and decoding audio
 */

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

#ifndef CELT_H
#define CELT_H

#include "opus_types.h"
#include "opus_defines.h"
#include "opus_custom.h"
#include "entenc.h"
#include "entdec.h"
#include "arch.h"
#include "kiss_fft.h"


#define CELTEncoder OpusCustomEncoder
#define CELTDecoder OpusCustomDecoder
#define CELTMode OpusCustomMode

typedef struct {
   int signalType;
   int offset;
} SILKInfo;


static OPUS_INLINE opus_int32 bits_to_bitrate(opus_int32 bits, opus_int32 Fs, opus_int32 frame_size) {
   return bits*(6*Fs/frame_size)/6;
}

static OPUS_INLINE opus_int32 bitrate_to_bits(opus_int32 bitrate, opus_int32 Fs, opus_int32 frame_size) {
   return bitrate*6/(6*Fs/frame_size);
}

/* Encoder stuff */

int celt_encoder_get_size(int channels);

int celt_encode_with_ec(OpusCustomEncoder * OPUS_RESTRICT st, const opus_res * pcm, int nbCompressedBytes, ec_enc *enc, char *scratch);

/* Scratch arena bytes (stack_alloc.h, 16-byte rounded allocations) for frames
   of 960 samples, two channels and the 21-band mode.
   Encoder: celt_encode_with_ec keeps in (2*1080*4), freq and X (2*960*4 each),
   five 2-channel band arrays, six band arrays and the collapse masks (25328),
   then quant_all_bands with theta RDO takes the band norms (2*800*4), six
   last-band saves (6*176*4), the byte save (1275) and alg_quant's leaf
   (179*4 + 2*176*4): 14032. Every earlier callee (MDCT 7680, ...) needs less
   at its call point. */
#define CELT_ENCODER_SCRATCH_BYTES 39360

int celt_encoder_init(CELTEncoder *st, int channels);


/* Decoder stuff */

int celt_decoder_get_size(int channels);


int celt_decoder_init(CELTDecoder *st, int channels);

int celt_decode_with_ec(OpusCustomDecoder * OPUS_RESTRICT st, const unsigned char *data,
      int len, opus_res * OPUS_RESTRICT pcm, int frame_size, ec_dec *dec, int accum, char *scratch);

/* Decoder: celt_decode_with_ec keeps X (2*960*4), six band arrays and the
   collapse masks (8304), then quant_all_bands takes the band norms (2*800*4)
   and one band leaf (176*4): 15408. Synthesis (960*4) needs less and
   deemphasis none; concealment runs before any allocation and peaks at X plus synthesis
   (11520). The coded channel count C comes from the packet, so a mono
   decoder needs the same bound. */
#define CELT_DECODER_SCRATCH_BYTES 15408

/* The encoder's and decoder's settings between frames: plain functions (the encoder and decoder structures are private
   to celt_encoder.c and celt_decoder.c), which the whole-program build inlines. Their arguments come from the Opus layer
   within range: channel counts 1 or 2, bands of the 21-band mode, complexity 1-10, loss 0-100. */
const CELTMode *celt_encoder_mode(const CELTEncoder *st);
void celt_encoder_set_complexity(CELTEncoder *st, int complexity);
void celt_encoder_set_loss(CELTEncoder *st, int loss_percent);
/* The coded channel count, the SILK layer's signal type and offset, and the bitrate left for CELT (rates of 500 bit/s
   or less leave the previous one) before celt_encode_with_ec() */
void celt_encoder_prepare(CELTEncoder *st, int stream_channels, const SILKInfo *info, opus_int32 bitrate);

const CELTMode *celt_decoder_mode(const CELTDecoder *st);
void celt_decoder_set_channels(CELTDecoder *st, int stream_channels);
void celt_decoder_set_start(CELTDecoder *st, int start_band);
void celt_decoder_set_end(CELTDecoder *st, int end_band);
void celt_decoder_reset(CELTDecoder *st);
opus_uint32 celt_decoder_final_range(const CELTDecoder *st);


#define OPUS_CUSTOM_NOSTATIC static OPUS_INLINE

static const unsigned char trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
/* Probs: NONE: 21.875%, LIGHT: 6.25%, NORMAL: 65.625%, AGGRESSIVE: 6.25% */
static const unsigned char spread_icdf[4] = {25, 23, 2, 0};

static const unsigned char tapset_icdf[3]={2,1,0};


#define COMBFILTER_MAXPERIOD 1024
#define COMBFILTER_MINPERIOD 15

extern const signed char tf_select_table[4][8];

#define VALIDATE_CELT_DECODER(st)

void comb_filter(opus_val32 *y, opus_val32 *x, int T0, int T1, int N,
      opus_val16 g0, opus_val16 g1, int tapset0, int tapset1,
      const celt_coef *window, int overlap);

void init_caps(const CELTMode *m,int *cap,int LM,int C);


#endif /* CELT_H */
