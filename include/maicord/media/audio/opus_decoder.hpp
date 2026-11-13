#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// MaiCord's Opus decoder (RFC 6716 with the RFC 8251 updates): every packet a peer or an Opus file can
// carry (SILK, CELT and hybrid frames of 2.5 to 60 ms, up to 120 ms per packet, mono or stereo, mode
// switches, in-band FEC, loss concealment) to 48 kHz PCM16 in the channel count the caller asked for,
// whatever the packet codes. No allocation: the state is this struct, and a call uses about 21 KB of the
// caller's stack.
inline constexpr u32 kOpusDecoderSampleRate = 48000;
inline constexpr u32 kOpusDecoderMaximumFrames = 5760; // 120 ms, the longest packet
inline constexpr u32 kOpusDecoderBytes = 26336;
struct OpusDecoder {
    alignas(32) u8 state[kOpusDecoderBytes];
};
// Starts a stream of 1 or 2 output channels (interleaved); `gain_q8` is the Ogg Opus header gain in 1/256
// dB, applied before the PCM16 clipping. Any content of the struct is overwritten.
void opus_decoder_start(OpusDecoder*, u32 channels, i32 gain_q8 = 0) noexcept;
// Forgets the decoded history, as after a seek; channels and gain stay.
void opus_decoder_reset(OpusDecoder*) noexcept;
// Decodes one packet into pcm and returns the frames (samples per channel) written, at most
// kOpusDecoderMaximumFrames. 0: the packet is malformed or longer than pcm; nothing was written and the
// state is unchanged, except for a frame that overruns its own bits (the caller conceals or resets either
// way).
u32 opus_decode(OpusDecoder*, ConstSlice<u8> packet, Slice<i16> pcm) noexcept;
// Fills in for the packet lost just before `next`: `frames` (a multiple of 120, at most 5760) are concealed
// from the decoder's history, or, when `next` carries a redundant copy of the lost frame (LBRR), decoded
// from it; the caller still decodes `next` afterwards. An empty `next` always conceals. 0 on a malformed
// `next` or a short pcm.
u32 opus_decode_lost(OpusDecoder*, ConstSlice<u8> next, u32 frames, Slice<i16> pcm) noexcept;
// True when the packet carries a redundant copy of the frame before it.
bool opus_packet_has_fec(ConstSlice<u8> packet) noexcept;
// The range coder's final state after the last packet, which equals the encoder's for a conformant decoder;
// 0 after a concealed frame. For tests.
u32 opus_decoder_final_range(const OpusDecoder*) noexcept;
} // namespace maicord
