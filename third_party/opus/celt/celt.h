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

#define celt_check_mode_ptr_ptr(ptr) ((ptr) + ((ptr) - (const CELTMode**)(ptr)))

#define celt_check_silkinfo_ptr(ptr) ((ptr) + ((ptr) - (const SILKInfo*)(ptr)))

/* Encoder/decoder Requests */


#define CELT_SET_PREDICTION_REQUEST    10002
/** Controls the use of interframe prediction.
    0=Independent frames
    1=Short term interframe prediction allowed
    2=Long term prediction allowed
 */
#define CELT_SET_PREDICTION(x) CELT_SET_PREDICTION_REQUEST, opus_check_int(x)

#define CELT_SET_CHANNELS_REQUEST    10008
#define CELT_SET_CHANNELS(x) CELT_SET_CHANNELS_REQUEST, opus_check_int(x)


/* Internal */
#define CELT_SET_START_BAND_REQUEST    10010
#define CELT_SET_START_BAND(x) CELT_SET_START_BAND_REQUEST, opus_check_int(x)

#define CELT_SET_END_BAND_REQUEST    10012
#define CELT_SET_END_BAND(x) CELT_SET_END_BAND_REQUEST, opus_check_int(x)

#define CELT_GET_MODE_REQUEST    10015
/** Get the CELTMode used by an encoder or decoder */
#define CELT_GET_MODE(x) CELT_GET_MODE_REQUEST, celt_check_mode_ptr_ptr(x)

#define CELT_SET_SILK_INFO_REQUEST    10028
#define CELT_SET_SILK_INFO(x) CELT_SET_SILK_INFO_REQUEST, celt_check_silkinfo_ptr(x)


static OPUS_INLINE opus_int32 bits_to_bitrate(opus_int32 bits, opus_int32 Fs, opus_int32 frame_size) {
   return bits*(6*Fs/frame_size)/6;
}

static OPUS_INLINE opus_int32 bitrate_to_bits(opus_int32 bitrate, opus_int32 Fs, opus_int32 frame_size) {
   return bitrate*6/(6*Fs/frame_size);
}

/* Encoder stuff */

int celt_encoder_get_size(int channels);

int celt_encode_with_ec(OpusCustomEncoder * OPUS_RESTRICT st, const opus_res * pcm, int frame_size, unsigned char *compressed, int nbCompressedBytes, ec_enc *enc);

int celt_encoder_init(CELTEncoder *st, int channels, int arch);


/* Decoder stuff */

int celt_decoder_get_size(int channels);


int celt_decoder_init(CELTDecoder *st, opus_int32 sampling_rate, int channels);

int celt_decode_with_ec(OpusCustomDecoder * OPUS_RESTRICT st, const unsigned char *data,
      int len, opus_res * OPUS_RESTRICT pcm, int frame_size, ec_dec *dec, int accum);

#define celt_encoder_ctl opus_custom_encoder_ctl
#define celt_decoder_ctl opus_custom_decoder_ctl


#define OPUS_CUSTOM_NOSTATIC static OPUS_INLINE

static const unsigned char trim_icdf[11] = {126, 124, 119, 109, 87, 41, 19, 9, 4, 2, 0};
/* Probs: NONE: 21.875%, LIGHT: 6.25%, NORMAL: 65.625%, AGGRESSIVE: 6.25% */
static const unsigned char spread_icdf[4] = {25, 23, 2, 0};

static const unsigned char tapset_icdf[3]={2,1,0};


#define COMBFILTER_MAXPERIOD 1024
#define COMBFILTER_MINPERIOD 15

extern const signed char tf_select_table[4][8];

void validate_celt_decoder(CELTDecoder *st);
#define VALIDATE_CELT_DECODER(st) validate_celt_decoder(st)

int resampling_factor(opus_int32 rate);

void comb_filter(opus_val32 *y, opus_val32 *x, int T0, int T1, int N,
      opus_val16 g0, opus_val16 g1, int tapset0, int tapset1,
      const celt_coef *window, int overlap, int arch);

void init_caps(const CELTMode *m,int *cap,int LM,int C);


#endif /* CELT_H */
