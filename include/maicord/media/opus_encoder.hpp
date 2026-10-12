#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Voice profile: 48 kHz PCM16, VOIP application, one 20 ms VBR frame per packet.
inline constexpr u32 kOpusEncoderSampleRate = 48000;
inline constexpr u32 kOpusEncoderFrameSamples = 960; // Per channel; stereo PCM is interleaved.
inline constexpr u32 kOpusEncoderStateAlignment = 16;
inline constexpr u32 kOpusEncoderMaximumPacketBytes = 1275; // RFC 6716 single-frame bound.
inline constexpr u32 kOpusEncoderMinimumBitrate = 6000, kOpusEncoderMaximumBitrate = 510000;
inline constexpr u32 kOpusEncoderDefaultBitrate = 64000;
inline constexpr u32 kOpusEncoderDefaultComplexity = 7, kOpusEncoderMaximumComplexity = 10;
inline constexpr u32 kOpusEncoderDefaultPacketLossPercent = 10;
// Stack headroom a thread must reserve for the _alloca scratch used by encode.
inline constexpr u32 kOpusEncoderStackReserve = 64u * 1024u;
enum class OpusEncodeError : u8 {
    none,
    invalid_input,
    invalid_state,
    busy,
    capacity,
    encoder
};
struct OpusEncodeResult {
    OpusEncodeError error{};
    i32 native_status{};
    u32 bytes{}; // Packets of at most 2 bytes carry no audio (DTX).
    constexpr bool ok() const noexcept {
        return error == OpusEncodeError::none;
    }
};
struct OpusEncoder {
    u8* state{};
    u32 state_bytes{}, channels{};
    bool initialized{};
};
// Same ownership contract as OpusDecoder: caller storage, one serial worker,
// no allocation or destruction by the wrapper. Initialization applies the
// defaults above with in-band FEC on and DTX off. Setters validate before the
// native call and leave state untouched on rejection; reset keeps settings.
u32 opus_encoder_storage_bytes(u32 channels) noexcept;
OpusEncodeResult opus_encoder_initialize(OpusEncoder*, Slice<u8> storage, u32 channels) noexcept;
OpusEncodeResult opus_encoder_reset(OpusEncoder*) noexcept;
OpusEncodeResult opus_encoder_bitrate(OpusEncoder*, u32 bits_per_second) noexcept;
OpusEncodeResult opus_encoder_complexity(OpusEncoder*, u32 complexity) noexcept;
OpusEncodeResult opus_encoder_inband_fec(OpusEncoder*, bool enabled) noexcept;
OpusEncodeResult opus_encoder_packet_loss(OpusEncoder*, u32 percent) noexcept;
OpusEncodeResult opus_encoder_dtx(OpusEncoder*, bool enabled) noexcept;
// Constrained VBR keeps every packet near the bitrate's average size (the
// profile always encodes VBR; CBR padding is not compiled in).
OpusEncodeResult opus_encoder_vbr_constraint(OpusEncoder*, bool enabled) noexcept;
// Encoder delay in 48 kHz samples (the Ogg Opus pre-skip).
OpusEncodeResult opus_encoder_lookahead(OpusEncoder*, u32* samples) noexcept;
// Exactly one frame (960 * channels samples) into at least
// kOpusEncoderMaximumPacketBytes of packet capacity.
OpusEncodeResult opus_encoder_encode(OpusEncoder*, ConstSlice<i16> pcm, Slice<u8> packet) noexcept;
void opus_encoder_release(OpusEncoder*) noexcept;
} // namespace maicord
