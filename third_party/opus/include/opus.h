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

/**
 * @file opus.h
 * @brief Opus reference implementation API
 */

#ifndef OPUS_H
#define OPUS_H

#include "opus_types.h"
#include "opus_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of caller scratch memory that opus_encode() and opus_decode() take:
  * the larger of the encoder's (44640) and the decoder's (19248) worst-case
  * working sets, asserted in src/opus_encoder.c and src/opus_decoder.c. */
#define OPUS_SCRATCH_BYTES 44640

/**
 * @mainpage Opus
 *
 * The Opus codec is designed for interactive speech and audio transmission over the Internet.
 * It is designed by the IETF Codec Working Group and incorporates technology from
 * Skype's SILK codec and Xiph.Org's CELT codec.
 *
 * The Opus codec is designed to handle a wide range of interactive audio applications,
 * including Voice over IP, videoconferencing, in-game chat, and even remote live music
 * performances. It can scale from low bit-rate narrowband speech to very high quality
 * stereo music. Its main features are:

 * maiopus is a fixed-point build of libopus with a complete decoder (every
 * mode, bandwidth, frame size, FEC and PLC, output at 8 to 48 kHz) and an
 * encoder reduced to one voice profile: 48 kHz mono or stereo input, VOIP
 * tuning, one 20 ms VBR frame per packet. The library never allocates and
 * never uses alloca: states live in caller memory sized by
 * opus_encoder_get_size() and opus_decoder_get_size(), and opus_encode() and
 * opus_decode() work in a caller-owned scratch block of #OPUS_SCRATCH_BYTES.
 *
 * Documentation sections:
 * @li @ref opus_encoder
 * @li @ref opus_decoder
 * @li @ref opus_libinfo
 */

/** @defgroup opus_encoder Opus Encoder
  * @{
  *
  * @brief This page describes the process and functions used to encode Opus.
  *
  * The encoder state lives in caller memory:
  *
  * @code
  * OpusEncoder *enc = malloc(opus_encoder_get_size(channels));
  * int error = opus_encoder_init(enc, 48000, channels, OPUS_APPLICATION_VOIP);
  * @endcode
  *
  * An encoder state @b must @b not be used for more than one stream at the same
  * time. It is continuous in memory, so a shallow copy is sufficient to copy it.
  *
  * Settings are changed with opus_encoder_ctl(), see @ref opus_encoderctls
  * for the supported requests.
  *
  * Each opus_encode() call takes exactly one 20 ms frame (960 samples per
  * channel) and returns the packet length, or a negative error code. Packets
  * of 2 bytes or less do not need to be transmitted (DTX).
  */

/** Opus encoder state.
  * This contains the complete state of an Opus encoder.
  * It is position independent and can be freely copied.
  * @see opus_encoder_init
  */
typedef struct OpusEncoder OpusEncoder;

/** Gets the size of an <code>OpusEncoder</code> structure.
  * @param[in] channels <tt>int</tt>: Number of channels.
  *                                   This must be 1 or 2.
  * @returns The size in bytes.
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_encoder_get_size(int channels);

/**
 */

/** Initializes a previously allocated encoder state
  * The memory pointed to by st must be at least the size returned by opus_encoder_get_size().
  * To reset a previously initialized state, use the #OPUS_RESET_STATE CTL.
  * @param [in] st <tt>OpusEncoder*</tt>: Encoder state
  * @param [in] Fs <tt>opus_int32</tt>: Sampling rate of input signal (Hz). This must be 48000.
  * @param [in] channels <tt>int</tt>: Number of channels (1 or 2) in input signal
  * @param [in] application <tt>int</tt>: Coding mode. This must be OPUS_APPLICATION_VOIP.
  * @retval #OPUS_OK Success or @ref opus_errorcodes
  */
OPUS_EXPORT int opus_encoder_init(
    OpusEncoder *st,
    opus_int32 Fs,
    int channels,
    int application
) OPUS_ARG_NONNULL(1);

/** Encodes an Opus frame.
  * @param [in] st <tt>OpusEncoder*</tt>: Encoder state
  * @param [in] pcm <tt>opus_int16*</tt>: Input signal (interleaved if 2 channels). length is frame_size*channels*sizeof(opus_int16)
  * @param [in] frame_size <tt>int</tt>: Number of samples per channel in the
  *                                      input signal. This must be 960 (20 ms).
  * @param [out] data <tt>unsigned char*</tt>: Output payload.
  *                                            This must contain storage for at
  *                                            least \a max_data_bytes.
  * @param [in] max_data_bytes <tt>opus_int32</tt>: Size of the allocated
  *                                                 memory for the output
  *                                                 payload. This may be
  *                                                 used to impose an upper limit on
  *                                                 the instant bitrate, but should
  *                                                 not be used as the only bitrate
  *                                                 control. Use #OPUS_SET_BITRATE to
  *                                                 control the bitrate.
  * @param [in] scratch <tt>void*</tt>: Caller-owned scratch memory of
  *                                     #OPUS_SCRATCH_BYTES, 16-byte aligned,
  *                                     not overlapping any other argument.
  *                                     It holds nothing between calls, so one
  *                                     block serves every encoder and decoder
  *                                     a thread runs.
  * @returns The length of the encoded packet (in bytes) on success or a
  *          negative error code (see @ref opus_errorcodes) on failure.
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT opus_int32 opus_encode(
    OpusEncoder *st,
    const opus_int16 *pcm,
    int frame_size,
    unsigned char *data,
    opus_int32 max_data_bytes,
    void *scratch
) OPUS_ARG_NONNULL(1) OPUS_ARG_NONNULL(2) OPUS_ARG_NONNULL(4) OPUS_ARG_NONNULL(6);

/** Perform a CTL function on an Opus encoder.
  *
  * Generally the request and subsequent arguments are generated
  * by a convenience macro.
  * @param st <tt>OpusEncoder*</tt>: Encoder state.
  * @param request This and all remaining parameters should be replaced by one
  *                of the convenience macros in @ref opus_genericctls or
  *                @ref opus_encoderctls.
  * @see opus_genericctls
  * @see opus_encoderctls
  */
OPUS_EXPORT int opus_encoder_ctl(OpusEncoder *st, int request, ...) OPUS_ARG_NONNULL(1);
/**@}*/

/** @defgroup opus_decoder Opus Decoder
  * @{
  *
  * @brief This page describes the process and functions used to decode Opus.
  *
  * The decoder state lives in caller memory:
  * @code
  * OpusDecoder *dec = malloc(opus_decoder_get_size(channels));
  * int error = opus_decoder_init(dec, Fs, channels);
  * @endcode
  * where Fs is the output sampling rate (8000, 12000, 16000, 24000, or 48000)
  * and channels is 1 or 2. The state is continuous in memory and only a
  * shallow copy is sufficient to copy it.
  *
  * To decode a frame, opus_decode() must be called with a packet of compressed audio data:
  * @code
  * frame_size = opus_decode(dec, packet, len, decoded, max_size, 0);
  * @endcode
  * where
  *
  * @li packet is the byte array containing the compressed data
  * @li len is the exact number of bytes contained in the packet
  * @li decoded is the decoded audio data in opus_int16
  * @li max_size is the max duration of the frame in samples (per channel) that can fit into the decoded_frame array
  *
  * opus_decode() returns the number of samples (per channel) decoded from the packet.
  * If that value is negative, then an error has occurred. This can occur if the packet is corrupted or if the audio
  * buffer is too small to hold the decoded audio.
  *
  * Opus is a stateful codec with overlapping blocks and as a result Opus
  * packets are not coded independently of each other. Packets must be
  * passed into the decoder serially and in the correct order for a correct
  * decode. Lost packets can be replaced with loss concealment by calling
  * the decoder with a null pointer and zero length for the missing packet.
  *
  * A single codec state may only be accessed from a single thread at
  * a time and any required locking must be performed by the caller. Separate
  * streams must be decoded with separate decoder states and can be decoded
  * in parallel.
  *
  */

/** Opus decoder state.
  * This contains the complete state of an Opus decoder.
  * It is position independent and can be freely copied.
  * @see opus_decoder_init
  */
typedef struct OpusDecoder OpusDecoder;

/** Gets the size of an <code>OpusDecoder</code> structure.
  * @param [in] channels <tt>int</tt>: Number of channels.
  *                                    This must be 1 or 2.
  * @returns The size in bytes.
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_decoder_get_size(int channels);

/** Initializes a previously allocated decoder state.
  * The state must be at least the size returned by opus_decoder_get_size().
  * @see opus_decoder_get_size
  * To reset a previously initialized state, use the #OPUS_RESET_STATE CTL.
  * @param [in] st <tt>OpusDecoder*</tt>: Decoder state.
  * @param [in] Fs <tt>opus_int32</tt>: Sampling rate to decode to (Hz).
  *                                     This must be one of 8000, 12000, 16000,
  *                                     24000, or 48000.
  * @param [in] channels <tt>int</tt>: Number of channels (1 or 2) to decode
  * @retval #OPUS_OK Success or @ref opus_errorcodes
  */
OPUS_EXPORT int opus_decoder_init(
    OpusDecoder *st,
    opus_int32 Fs,
    int channels
) OPUS_ARG_NONNULL(1);

/** Decode an Opus packet.
  * @param [in] st <tt>OpusDecoder*</tt>: Decoder state
  * @param [in] data <tt>char*</tt>: Input payload. Use a NULL pointer to indicate packet loss
  * @param [in] len <tt>opus_int32</tt>: Number of bytes in payload*
  * @param [out] pcm <tt>opus_int16*</tt>: Output signal (interleaved if 2 channels). length
  *  is frame_size*channels*sizeof(opus_int16)
  * @param [in] frame_size Number of samples per channel of available space in \a pcm.
  *  If this is less than the maximum packet duration (120ms; 5760 for 48kHz), this function will
  *  not be capable of decoding some packets. In the case of PLC (data==NULL) or FEC (decode_fec=1),
  *  then frame_size needs to be exactly the duration of audio that is missing, otherwise the
  *  decoder will not be in the optimal state to decode the next incoming packet. For the PLC and
  *  FEC cases, frame_size <b>must</b> be a multiple of 2.5 ms.
  * @param [in] decode_fec <tt>int</tt>: Flag (0 or 1) to request that any in-band forward error correction data be
  *  decoded. If no such data is available, the frame is decoded as if it were lost.
  * @param [in] scratch <tt>void*</tt>: Caller-owned scratch memory of
  *  #OPUS_SCRATCH_BYTES, 16-byte aligned, not overlapping any other argument
  *  (see opus_encode()).
  * @returns Number of decoded samples per channel or @ref opus_errorcodes
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_decode(
    OpusDecoder *st,
    const unsigned char *data,
    opus_int32 len,
    opus_int16 *pcm,
    int frame_size,
    int decode_fec,
    void *scratch
) OPUS_ARG_NONNULL(1) OPUS_ARG_NONNULL(4) OPUS_ARG_NONNULL(7);

/** Perform a CTL function on an Opus decoder.
  *
  * Generally the request and subsequent arguments are generated
  * by a convenience macro.
  * @param st <tt>OpusDecoder*</tt>: Decoder state.
  * @param request This and all remaining parameters should be replaced by one
  *                of the convenience macros in @ref opus_genericctls or
  *                @ref opus_decoderctls.
  * @see opus_genericctls
  * @see opus_decoderctls
  */
OPUS_EXPORT int opus_decoder_ctl(OpusDecoder *st, int request, ...) OPUS_ARG_NONNULL(1);

/** Parse an opus packet into one or more frames.
  * Opus_decode will perform this operation internally so most applications do
  * not need to use this function.
  * This function does not copy the frames, the returned pointers are pointers into
  * the input packet.
  * @param [in] data <tt>char*</tt>: Opus packet to be parsed
  * @param [in] len <tt>opus_int32</tt>: size of data
  * @param [out] out_toc <tt>char*</tt>: TOC pointer
  * @param [out] frames <tt>char*[48]</tt> encapsulated frames
  * @param [out] size <tt>opus_int16[48]</tt> sizes of the encapsulated frames
  * @param [out] payload_offset <tt>int*</tt>: returns the position of the payload within the packet (in bytes)
  * @returns number of frames
  */
OPUS_EXPORT int opus_packet_parse(
   const unsigned char *data,
   opus_int32 len,
   unsigned char *out_toc,
   const unsigned char *frames[48],
   opus_int16 size[48],
   int *payload_offset
) OPUS_ARG_NONNULL(1) OPUS_ARG_NONNULL(5);

/** Gets the bandwidth of an Opus packet.
  * @param [in] data <tt>char*</tt>: Opus packet
  * @retval OPUS_BANDWIDTH_NARROWBAND Narrowband (4kHz bandpass)
  * @retval OPUS_BANDWIDTH_MEDIUMBAND Mediumband (6kHz bandpass)
  * @retval OPUS_BANDWIDTH_WIDEBAND Wideband (8kHz bandpass)
  * @retval OPUS_BANDWIDTH_SUPERWIDEBAND Superwideband (12kHz bandpass)
  * @retval OPUS_BANDWIDTH_FULLBAND Fullband (20kHz bandpass)
  * @retval OPUS_INVALID_PACKET The compressed data passed is corrupted or of an unsupported type
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_get_bandwidth(const unsigned char *data) OPUS_ARG_NONNULL(1);

/** Gets the number of samples per frame from an Opus packet.
  * @param [in] data <tt>char*</tt>: Opus packet.
  *                                  This must contain at least one byte of
  *                                  data.
  * @param [in] Fs <tt>opus_int32</tt>: Sampling rate in Hz.
  *                                     This must be a multiple of 400, or
  *                                     inaccurate results will be returned.
  * @returns Number of samples per frame.
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_get_samples_per_frame(const unsigned char *data, opus_int32 Fs) OPUS_ARG_NONNULL(1);

/** Gets the number of channels from an Opus packet.
  * @param [in] data <tt>char*</tt>: Opus packet
  * @returns Number of channels
  * @retval OPUS_INVALID_PACKET The compressed data passed is corrupted or of an unsupported type
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_get_nb_channels(const unsigned char *data) OPUS_ARG_NONNULL(1);

/** Gets the number of frames in an Opus packet.
  * @param [in] packet <tt>char*</tt>: Opus packet
  * @param [in] len <tt>opus_int32</tt>: Length of packet
  * @returns Number of frames
  * @retval OPUS_BAD_ARG Insufficient data was passed to the function
  * @retval OPUS_INVALID_PACKET The compressed data passed is corrupted or of an unsupported type
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len) OPUS_ARG_NONNULL(1);

/** Gets the number of samples of an Opus packet.
  * @param [in] packet <tt>char*</tt>: Opus packet
  * @param [in] len <tt>opus_int32</tt>: Length of packet
  * @param [in] Fs <tt>opus_int32</tt>: Sampling rate in Hz.
  *                                     This must be a multiple of 400, or
  *                                     inaccurate results will be returned.
  * @returns Number of samples
  * @retval OPUS_BAD_ARG Insufficient data was passed to the function
  * @retval OPUS_INVALID_PACKET The compressed data passed is corrupted or of an unsupported type
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_get_nb_samples(const unsigned char packet[], opus_int32 len, opus_int32 Fs) OPUS_ARG_NONNULL(1);

/** Checks whether an Opus packet has LBRR.
  * @param [in] packet <tt>char*</tt>: Opus packet
  * @param [in] len <tt>opus_int32</tt>: Length of packet
  * @returns 1 is LBRR is present, 0 otherwise
  * @retval OPUS_INVALID_PACKET The compressed data passed is corrupted or of an unsupported type
  */
OPUS_EXPORT OPUS_WARN_UNUSED_RESULT int opus_packet_has_lbrr(const unsigned char packet[], opus_int32 len);

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* OPUS_H */
