#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/media/opus_scratch.hpp"

namespace maicord {
inline constexpr u32 kOpusDecoderSampleRate = 48000;
inline constexpr u32 kOpusDecoderMaximumFrames = 5760;
inline constexpr u32 kOpusDecoderStateAlignment = 16;
// Local admission policy, including padding; not a claimed bitstream maximum.
inline constexpr u32 kOpusDecoderMaximumPacketBytes = 65536;
enum class OpusDecodeError : u8 {
    none,
    invalid_input,
    invalid_state,
    busy,
    capacity,
    packet,
    decoder
};
struct OpusDecodeResult {
    OpusDecodeError error{};
    i32 native_status{};
    u32 frames{}; // Per channel; PCM is interleaved for stereo.
    constexpr bool ok() const noexcept {
        return error == OpusDecodeError::none;
    }
};
struct OpusDecoder {
    u8* state{};
    u32 state_bytes{}, channels{};
    bool initialized{};
};
// No allocation or destruction by the wrapper. Storage must stay alive and
// exclusively owned by the stream until release; one serial worker uses it.
u32 opus_decoder_storage_bytes(u32 channels) noexcept;
OpusDecodeResult opus_decoder_initialize(OpusDecoder*, Slice<u8> storage, u32 channels) noexcept;
OpusDecodeResult opus_decoder_reset(OpusDecoder*) noexcept;
// The session chooses the header gain; Opus applies Q8dB gain before PCM16
// clipping. Device/user volume remains a separate session policy.
OpusDecodeResult opus_decoder_gain(OpusDecoder*, i16 gain_q8) noexcept;
// Complete packets only: empty input is an error, never implicit concealment.
// Capacity/argument rejection precedes state mutation. Native packet failure
// is explicit; the playback owner decides whether to reset or end the stream.
// `scratch` is the calling thread's (see opus_scratch.hpp); it must not overlap
// the state, packet or PCM.
OpusDecodeResult opus_decoder_decode(OpusDecoder*, ConstSlice<u8> packet, Slice<i16> pcm, OpusScratch* scratch) noexcept;
// Packet-loss concealment for one missing packet: synthesizes `frames` (a
// 2.5 ms multiple, at most 120 ms) from decoder history. Same preflight rules.
OpusDecodeResult opus_decoder_conceal(OpusDecoder*, u32 frames, Slice<i16> pcm, OpusScratch* scratch) noexcept;
// Recovers the packet lost immediately before `next` from next's in-band FEC
// (SILK LBRR); without LBRR libopus conceals instead. The caller still decodes
// `next` normally afterwards.
OpusDecodeResult opus_decoder_decode_fec(OpusDecoder*, ConstSlice<u8> next, u32 frames,
                                         Slice<i16> pcm, OpusScratch* scratch) noexcept;
bool opus_packet_has_fec(ConstSlice<u8> packet) noexcept;
void opus_decoder_release(OpusDecoder*) noexcept;
} // namespace maicord
