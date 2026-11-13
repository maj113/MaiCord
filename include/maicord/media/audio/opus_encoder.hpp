#pragma once
#include "maicord/core/base.hpp"

namespace maicord {
// MaiCord's Opus encoder (RFC 6716): 48 kHz PCM16, mono or stereo, one 20 ms frame per packet, always coded
// as full-band hybrid (SILK wideband plus CELT above 8 kHz) at a variable rate around the profile's. No
// allocation: the state is this struct, and a call uses about 34 KB of the caller's stack.
inline constexpr u32 kOpusEncoderSampleRate = 48000;
inline constexpr u32 kOpusEncoderFrameSamples = 960;        // per channel; stereo PCM is interleaved
inline constexpr u32 kOpusEncoderMaximumPacketBytes = 1275; // RFC 6716's bound for one frame
inline constexpr u32 kOpusEncoderLookahead =
    312; // samples the decoded signal lags the input (Ogg pre-skip)
struct OpusEncoderProfile {
    u32 bitrate = 64000; // 6000 to 510000 bits per second
    u8 channels = 1;
    u8 complexity = 7; // 1 to 10: how hard the encoder searches
    // Expected packet loss, 0 to 100. Above 0 a packet also carries a coarse copy of the frame before it
    // (in-band FEC) when the rate allows, and the coding leans less on what a lost packet would take with
    // it.
    u8 loss_percent = 10;
};
inline constexpr u32 kOpusEncoderBytes = 20896;
struct OpusEncoder {
    alignas(32) u8 state[kOpusEncoderBytes];
};
// Starts a stream; out-of-range profile values are clamped. Any content of the struct is overwritten.
void opus_encoder_start(OpusEncoder*, const OpusEncoderProfile&) noexcept;
// Encodes kOpusEncoderFrameSamples per channel into `packet` (kOpusEncoderMaximumPacketBytes of room) and
// returns the packet's bytes: 2 for a frame that carries no audio (the decoder conceals), more otherwise.
u32 opus_encode(OpusEncoder*, const i16* pcm, u8* packet) noexcept;
// The range coder's final state of the last packet, which a decoder of it must reach too. For tests.
u32 opus_encoder_final_range(const OpusEncoder*) noexcept;
} // namespace maicord
