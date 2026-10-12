#include "maicord/media/opus_decoder.hpp"
#include <opus.h>

namespace maicord {
namespace {
static_assert(sizeof(i16) == sizeof(opus_int16));
OpusDecodeResult result(OpusDecodeError error, i32 native = OPUS_OK, u32 frames = 0) noexcept {
    return {error, native, frames};
}
bool overlaps(const void* a, usize a_bytes, const void* b, usize b_bytes) noexcept {
    if (!a_bytes || !b_bytes)
        return false;
    const auto first = reinterpret_cast<usize>(a), second = reinterpret_cast<usize>(b);
    return first <= second ? second - first < a_bytes : first - second < b_bytes;
}
bool current(const OpusDecoder* decoder) noexcept {
    return decoder && decoder->initialized && decoder->state &&
           !(reinterpret_cast<usize>(decoder->state) & (kOpusDecoderStateAlignment - 1)) &&
           decoder->state_bytes && decoder->state_bytes == opus_decoder_storage_bytes(decoder->channels);
}
::OpusDecoder* native_state(OpusDecoder* decoder) noexcept {
    return reinterpret_cast<::OpusDecoder*>(decoder->state);
}
} // namespace

u32 opus_decoder_storage_bytes(u32 channels) noexcept {
    if (channels != 1 && channels != 2)
        return 0;
    const int bytes = ::opus_decoder_get_size(static_cast<int>(channels));
    return bytes > 0 ? static_cast<u32>(bytes) : 0;
}
OpusDecodeResult opus_decoder_initialize(OpusDecoder* decoder, Slice<u8> storage, u32 channels) noexcept {
    if (!decoder)
        return result(OpusDecodeError::invalid_input, OPUS_BAD_ARG);
    if (decoder->initialized || decoder->state)
        return result(OpusDecodeError::busy, OPUS_INVALID_STATE);
    const u32 bytes = opus_decoder_storage_bytes(channels);
    if (!bytes || !storage.data ||
        (reinterpret_cast<usize>(storage.data) & (kOpusDecoderStateAlignment - 1)) ||
        overlaps(decoder, sizeof(*decoder), storage.data, bytes))
        return result(OpusDecodeError::invalid_input, OPUS_BAD_ARG);
    if (storage.count < bytes)
        return result(OpusDecodeError::capacity, OPUS_BUFFER_TOO_SMALL);
    const int status =
        ::opus_decoder_init(reinterpret_cast<::OpusDecoder*>(storage.data),
                            static_cast<opus_int32>(kOpusDecoderSampleRate), static_cast<int>(channels));
    if (status != OPUS_OK)
        return result(OpusDecodeError::decoder, status);
    *decoder = {storage.data, bytes, channels, true};
    return result(OpusDecodeError::none);
}
OpusDecodeResult opus_decoder_reset(OpusDecoder* decoder) noexcept {
    if (!current(decoder))
        return result(OpusDecodeError::invalid_state, OPUS_INVALID_STATE);
    const int status = ::opus_decoder_ctl(native_state(decoder), OPUS_RESET_STATE);
    return result(status == OPUS_OK ? OpusDecodeError::none : OpusDecodeError::decoder, status);
}
OpusDecodeResult opus_decoder_gain(OpusDecoder* decoder, i16 gain_q8) noexcept {
    if (!current(decoder))
        return result(OpusDecodeError::invalid_state, OPUS_INVALID_STATE);
    const int status = ::opus_decoder_ctl(native_state(decoder), OPUS_SET_GAIN(static_cast<int>(gain_q8)));
    return result(status == OPUS_OK ? OpusDecodeError::none : OpusDecodeError::decoder, status);
}
OpusDecodeResult opus_decoder_decode(OpusDecoder* decoder, ConstSlice<u8> packet, Slice<i16> pcm) noexcept {
    if (!current(decoder))
        return result(OpusDecodeError::invalid_state, OPUS_INVALID_STATE);
    if (!packet.data || !packet.count || packet.count > kOpusDecoderMaximumPacketBytes || !pcm.data ||
        (reinterpret_cast<usize>(pcm.data) & (alignof(i16) - 1)) ||
        overlaps(decoder->state, decoder->state_bytes, packet.data, packet.count) ||
        overlaps(decoder->state, decoder->state_bytes, pcm.data,
                 static_cast<usize>(pcm.count) * sizeof(i16)) ||
        overlaps(packet.data, packet.count, pcm.data, static_cast<usize>(pcm.count) * sizeof(i16)) ||
        overlaps(decoder, sizeof(*decoder), pcm.data, static_cast<usize>(pcm.count) * sizeof(i16)))
        return result(OpusDecodeError::invalid_input, OPUS_BAD_ARG);
    const int frames = ::opus_packet_get_nb_samples(packet.data, static_cast<opus_int32>(packet.count),
                                                    static_cast<opus_int32>(kOpusDecoderSampleRate));
    if (frames <= 0 || frames > static_cast<int>(kOpusDecoderMaximumFrames))
        return result(OpusDecodeError::packet, frames < 0 ? frames : OPUS_INVALID_PACKET);
    if (pcm.count / decoder->channels < static_cast<u32>(frames))
        return result(OpusDecodeError::capacity, OPUS_BUFFER_TOO_SMALL);
    const int decoded = ::opus_decode(native_state(decoder), packet.data,
                                      static_cast<opus_int32>(packet.count), pcm.data, frames, 0);
    if (decoded < 0)
        return result(decoded == OPUS_INVALID_PACKET ? OpusDecodeError::packet : OpusDecodeError::decoder,
                      decoded);
    if (decoded != frames)
        return result(OpusDecodeError::decoder, OPUS_INTERNAL_ERROR);
    return result(OpusDecodeError::none, OPUS_OK, static_cast<u32>(decoded));
}
namespace {
// Shared by concealment (no packet) and FEC (the following packet).
OpusDecodeResult recover(OpusDecoder* decoder, ConstSlice<u8> next, u32 frames, Slice<i16> pcm) noexcept {
    if (!current(decoder))
        return result(OpusDecodeError::invalid_state, OPUS_INVALID_STATE);
    const usize pcm_bytes = static_cast<usize>(pcm.count) * sizeof(i16);
    if (!frames || frames > kOpusDecoderMaximumFrames || frames % (kOpusDecoderSampleRate / 400) || !pcm.data ||
        (reinterpret_cast<usize>(pcm.data) & (alignof(i16) - 1)) ||
        overlaps(decoder->state, decoder->state_bytes, pcm.data, pcm_bytes) ||
        overlaps(decoder, sizeof(*decoder), pcm.data, pcm_bytes) ||
        (next.data && (next.count > kOpusDecoderMaximumPacketBytes ||
                       overlaps(decoder->state, decoder->state_bytes, next.data, next.count) ||
                       overlaps(next.data, next.count, pcm.data, pcm_bytes))))
        return result(OpusDecodeError::invalid_input, OPUS_BAD_ARG);
    if (pcm.count / decoder->channels < frames)
        return result(OpusDecodeError::capacity, OPUS_BUFFER_TOO_SMALL);
    const bool fec = next.data && next.count;
    const int decoded = ::opus_decode(native_state(decoder), fec ? next.data : nullptr,
                                      fec ? static_cast<opus_int32>(next.count) : 0, pcm.data,
                                      static_cast<int>(frames), fec ? 1 : 0);
    if (decoded < 0)
        return result(decoded == OPUS_INVALID_PACKET ? OpusDecodeError::packet : OpusDecodeError::decoder,
                      decoded);
    if (static_cast<u32>(decoded) != frames)
        return result(OpusDecodeError::decoder, OPUS_INTERNAL_ERROR);
    return result(OpusDecodeError::none, OPUS_OK, frames);
}
} // namespace
OpusDecodeResult opus_decoder_conceal(OpusDecoder* decoder, u32 frames, Slice<i16> pcm) noexcept {
    return recover(decoder, {}, frames, pcm);
}
OpusDecodeResult opus_decoder_decode_fec(OpusDecoder* decoder, ConstSlice<u8> next, u32 frames,
                                         Slice<i16> pcm) noexcept {
    if (!next.data || !next.count)
        return result(OpusDecodeError::invalid_input, OPUS_BAD_ARG);
    return recover(decoder, next, frames, pcm);
}
bool opus_packet_has_fec(ConstSlice<u8> packet) noexcept {
    return packet.data && packet.count && packet.count <= kOpusDecoderMaximumPacketBytes &&
           ::opus_packet_has_lbrr(packet.data, static_cast<opus_int32>(packet.count)) > 0;
}
void opus_decoder_release(OpusDecoder* decoder) noexcept {
    if (decoder)
        *decoder = {};
}
} // namespace maicord
