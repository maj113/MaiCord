#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/media/audio/aac_configuration.hpp"

namespace maicord {
inline constexpr u32 kAacDecoderMaximumPacketBytes = 16384;
inline constexpr u32 kAacDecoderMaximumOutputBytes = 16384;
// Windows' decoder advertises a 49152-byte native output minimum even for LC
// mono/stereo. Native reusable capacity is separate from the PCM block size.
inline constexpr u32 kAacDecoderMaximumNativeOutputBytes = 65536;
// HE-AAC's periodic SBR headers are not present in every packet. Cover a
// typical one-second header interval plus filter history before audible PCM.
// 32 packets cover >=1.36 s at the supported HE rates; LC only needs overlap.
inline constexpr u32 aac_decoder_seek_preroll_packets(u32 frames_per_packet) noexcept {
    return frames_per_packet == 2048 ? 32u : 4u;
}
enum class AacDecodeError : u8 {
    none,
    invalid_input,
    unsupported,
    busy,
    runtime,
    media_type,
    input,
    output,
    capacity,
    need_input,
    need_output,
    end_of_stream,
    thread,
    release
};
struct AacDecodeResult {
    AacDecodeError error;
    i32 native_status;
    bool ok() const noexcept {
        return error == AacDecodeError::none;
    }
};
struct AacDecodedBlock {
    AacDecodeError error;
    i32 native_status;
    u32 bytes, frames;
    i64 presentation_time_100ns;
    bool timestamp_valid;
    bool ok() const noexcept {
        return error == AacDecodeError::none;
    }
};
struct AacDecoder {
    void* runtime;
    u8 asc[16];
    u32 asc_bytes, sample_rate, channels, thread_id;
    i32 native_status;
    u64 accepted_frames, output_frames;
    bool initialized, draining, ended;
};
// Pure metadata init: copies only <=16 ASC bytes. AAC-LC or signaled
// SBR/PS, output mono/stereo and 1024/2048 PCM frames. No PCE/core dependency or
// 960-frame mode. Rate/channels are post-SBR/PS output, not native input values.
// No COM/MF/allocation/device/thread.
inline bool aac_decoder_configuration_valid(ConstSlice<u8> asc, u32 rate, u32 channels) noexcept {
    AacConfiguration configuration{};
    return aac_configuration_decode(asc, &configuration) && configuration.sample_rate == rate &&
           configuration.channels == channels;
}
// MFT stream requirements are byte alignments; MF aligned-buffer creation uses
// a power-of-two-minus-one mask. Zero/one means no stronger alignment required.
// Reject non-power-of-two or >4096 byte requirements without changing output.
inline bool aac_decoder_alignment_mask(u32 alignment, u32* mask) noexcept {
    if (!mask || alignment > 4096 || (alignment > 1 && (alignment & (alignment - 1))))
        return false;
    *mask = alignment > 1 ? alignment - 1 : 0;
    return true;
}
inline bool aac_decoder_output_capacity(u32 minimum, u32* capacity) noexcept {
    if (!capacity || minimum > kAacDecoderMaximumNativeOutputBytes)
        return false;
    *capacity = minimum > kAacDecoderMaximumOutputBytes ? minimum : kAacDecoderMaximumOutputBytes;
    return true;
}
AacDecodeResult aac_decoder_initialize(AacDecoder*, ConstSlice<u8> asc, u32 rate, u32 channels) noexcept;
// Decode-worker only and serial. Caller owns an initialized MTA on that thread
// through release; this wrapper does not change the caller's COM apartment.
// First submit lazily opens the system MFT and balances its own MFStartup/LITE
// with MFShutdown. No SourceReader, device or worker is created. Packet/sample
// bounds exclude native MFT/COM/process heap allocation.
// The reusable native output buffer may be up to 64KiB to satisfy its advertised
// minimum; each returned PCM block still must fit the 16KiB caller/output bound.
// INT64_MIN is reserved as the internal missing-timestamp sentinel.
AacDecodeResult aac_decoder_submit(AacDecoder*, ConstSlice<u8> raw_frame, i64 presentation_time_100ns,
                                   u64 duration_100ns) noexcept;
// Caller output must fit maximum output bytes; insufficient capacity does not
// consume decoded output. Signed native timestamps are reported when present;
// no guessed priming, padding, edit-list trim or missing timestamp is invented.
AacDecodedBlock aac_decoder_read(AacDecoder*, Slice<u8> pcm) noexcept;
AacDecodeResult aac_decoder_drain(AacDecoder*) noexcept;
AacDecodeResult aac_decoder_flush(AacDecoder*) noexcept;
AacDecodeResult aac_decoder_release(AacDecoder*) noexcept;
} // namespace maicord
