#include "maicord/media/opus_encoder.hpp"
#include <opus.h>

namespace maicord {
namespace {
static_assert(sizeof(i16) == sizeof(opus_int16));
OpusEncodeResult result(OpusEncodeError error, i32 native = OPUS_OK, u32 bytes = 0) noexcept {
    return {error, native, bytes};
}
bool overlaps(const void* a, usize a_bytes, const void* b, usize b_bytes) noexcept {
    if (!a_bytes || !b_bytes)
        return false;
    const auto first = reinterpret_cast<usize>(a), second = reinterpret_cast<usize>(b);
    return first <= second ? second - first < a_bytes : first - second < b_bytes;
}
bool current(const OpusEncoder* encoder) noexcept {
    return encoder && encoder->initialized && encoder->state &&
           !(reinterpret_cast<usize>(encoder->state) & (kOpusEncoderStateAlignment - 1)) &&
           encoder->state_bytes && encoder->state_bytes == opus_encoder_storage_bytes(encoder->channels);
}
::OpusEncoder* native_state(OpusEncoder* encoder) noexcept {
    return reinterpret_cast<::OpusEncoder*>(encoder->state);
}
OpusEncodeResult control(OpusEncoder* encoder, int request, u32 value, u32 minimum, u32 maximum) noexcept {
    if (!current(encoder))
        return result(OpusEncodeError::invalid_state, OPUS_INVALID_STATE);
    if (value < minimum || value > maximum)
        return result(OpusEncodeError::invalid_input, OPUS_BAD_ARG);
    const int status = ::opus_encoder_ctl(native_state(encoder), request, static_cast<opus_int32>(value));
    return result(status == OPUS_OK ? OpusEncodeError::none : OpusEncodeError::encoder, status);
}
} // namespace

u32 opus_encoder_storage_bytes(u32 channels) noexcept {
    if (channels != 1 && channels != 2)
        return 0;
    const int bytes = ::opus_encoder_get_size(static_cast<int>(channels));
    return bytes > 0 ? static_cast<u32>(bytes) : 0;
}
OpusEncodeResult opus_encoder_initialize(OpusEncoder* encoder, Slice<u8> storage, u32 channels) noexcept {
    if (!encoder)
        return result(OpusEncodeError::invalid_input, OPUS_BAD_ARG);
    if (encoder->initialized || encoder->state)
        return result(OpusEncodeError::busy, OPUS_INVALID_STATE);
    const u32 bytes = opus_encoder_storage_bytes(channels);
    if (!bytes || !storage.data ||
        (reinterpret_cast<usize>(storage.data) & (kOpusEncoderStateAlignment - 1)) ||
        overlaps(encoder, sizeof(*encoder), storage.data, bytes))
        return result(OpusEncodeError::invalid_input, OPUS_BAD_ARG);
    if (storage.count < bytes)
        return result(OpusEncodeError::capacity, OPUS_BUFFER_TOO_SMALL);
    auto* native = reinterpret_cast<::OpusEncoder*>(storage.data);
    int status = ::opus_encoder_init(native, static_cast<opus_int32>(kOpusEncoderSampleRate),
                                     static_cast<int>(channels), OPUS_APPLICATION_VOIP);
    if (status == OPUS_OK)
        status = ::opus_encoder_ctl(native, OPUS_SET_BITRATE(static_cast<opus_int32>(kOpusEncoderDefaultBitrate)));
    if (status == OPUS_OK)
        status = ::opus_encoder_ctl(native, OPUS_SET_COMPLEXITY(static_cast<int>(kOpusEncoderDefaultComplexity)));
    if (status == OPUS_OK)
        status = ::opus_encoder_ctl(native, OPUS_SET_INBAND_FEC(1));
    if (status == OPUS_OK)
        status = ::opus_encoder_ctl(native, OPUS_SET_PACKET_LOSS_PERC(static_cast<int>(kOpusEncoderDefaultPacketLossPercent)));
    if (status != OPUS_OK)
        return result(OpusEncodeError::encoder, status);
    *encoder = {storage.data, bytes, channels, true};
    return result(OpusEncodeError::none);
}
OpusEncodeResult opus_encoder_reset(OpusEncoder* encoder) noexcept {
    if (!current(encoder))
        return result(OpusEncodeError::invalid_state, OPUS_INVALID_STATE);
    const int status = ::opus_encoder_ctl(native_state(encoder), OPUS_RESET_STATE);
    return result(status == OPUS_OK ? OpusEncodeError::none : OpusEncodeError::encoder, status);
}
OpusEncodeResult opus_encoder_bitrate(OpusEncoder* encoder, u32 bits_per_second) noexcept {
    return control(encoder, OPUS_SET_BITRATE_REQUEST, bits_per_second, kOpusEncoderMinimumBitrate,
                   kOpusEncoderMaximumBitrate);
}
OpusEncodeResult opus_encoder_complexity(OpusEncoder* encoder, u32 complexity) noexcept {
    return control(encoder, OPUS_SET_COMPLEXITY_REQUEST, complexity, 0, kOpusEncoderMaximumComplexity);
}
OpusEncodeResult opus_encoder_inband_fec(OpusEncoder* encoder, bool enabled) noexcept {
    return control(encoder, OPUS_SET_INBAND_FEC_REQUEST, enabled, 0, 1);
}
OpusEncodeResult opus_encoder_packet_loss(OpusEncoder* encoder, u32 percent) noexcept {
    return control(encoder, OPUS_SET_PACKET_LOSS_PERC_REQUEST, percent, 0, 100);
}
OpusEncodeResult opus_encoder_dtx(OpusEncoder* encoder, bool enabled) noexcept {
    return control(encoder, OPUS_SET_DTX_REQUEST, enabled, 0, 1);
}
OpusEncodeResult opus_encoder_vbr_constraint(OpusEncoder* encoder, bool enabled) noexcept {
    return control(encoder, OPUS_SET_VBR_CONSTRAINT_REQUEST, enabled, 0, 1);
}
OpusEncodeResult opus_encoder_lookahead(OpusEncoder* encoder, u32* samples) noexcept {
    if (!current(encoder))
        return result(OpusEncodeError::invalid_state, OPUS_INVALID_STATE);
    if (!samples)
        return result(OpusEncodeError::invalid_input, OPUS_BAD_ARG);
    opus_int32 value = 0;
    const int status = ::opus_encoder_ctl(native_state(encoder), OPUS_GET_LOOKAHEAD(&value));
    if (status != OPUS_OK || value < 0)
        return result(OpusEncodeError::encoder, status);
    *samples = static_cast<u32>(value);
    return result(OpusEncodeError::none);
}
OpusEncodeResult opus_encoder_encode(OpusEncoder* encoder, ConstSlice<i16> pcm, Slice<u8> packet) noexcept {
    if (!current(encoder))
        return result(OpusEncodeError::invalid_state, OPUS_INVALID_STATE);
    const usize pcm_bytes = static_cast<usize>(pcm.count) * sizeof(i16);
    if (!pcm.data || pcm.count != kOpusEncoderFrameSamples * encoder->channels ||
        (reinterpret_cast<usize>(pcm.data) & (alignof(i16) - 1)) || !packet.data ||
        overlaps(encoder->state, encoder->state_bytes, pcm.data, pcm_bytes) ||
        overlaps(encoder->state, encoder->state_bytes, packet.data, packet.count) ||
        overlaps(pcm.data, pcm_bytes, packet.data, packet.count) ||
        overlaps(encoder, sizeof(*encoder), packet.data, packet.count))
        return result(OpusEncodeError::invalid_input, OPUS_BAD_ARG);
    if (packet.count < kOpusEncoderMaximumPacketBytes)
        return result(OpusEncodeError::capacity, OPUS_BUFFER_TOO_SMALL);
    const opus_int32 bytes = ::opus_encode(native_state(encoder), pcm.data, static_cast<int>(kOpusEncoderFrameSamples),
                                           packet.data, static_cast<opus_int32>(kOpusEncoderMaximumPacketBytes));
    if (bytes <= 0 || bytes > static_cast<opus_int32>(kOpusEncoderMaximumPacketBytes))
        return result(OpusEncodeError::encoder, bytes < 0 ? bytes : OPUS_INTERNAL_ERROR);
    return result(OpusEncodeError::none, OPUS_OK, static_cast<u32>(bytes));
}
void opus_encoder_release(OpusEncoder* encoder) noexcept {
    if (encoder)
        *encoder = {};
}
} // namespace maicord
