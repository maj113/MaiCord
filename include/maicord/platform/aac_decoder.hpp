#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
inline constexpr u32 kAacDecoderMaximumPacketBytes = 16384;
inline constexpr u32 kAacDecoderMaximumOutputBytes = 16384;
// Windows' decoder advertises a 49152-byte native output minimum even for LC
// mono/stereo. Native reusable capacity is separate from the PCM block size.
inline constexpr u32 kAacDecoderMaximumNativeOutputBytes = 65536;
enum class AacPcmFormat : u8 {
    signed16,
    float32
};
enum class AacNativeStage : u8 {
    none,
    runtime,
    decoder,
    input_type,
    output_type,
    output_info,
    output_samples,
    input_info,
    streaming,
    input,
    output,
    drain,
    flush,
    release
};
enum class AacDecodeError : u8 {
    none,
    invalid_input,
    unsupported,
    busy,
    runtime,
    memory,
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
    AacDecodeError error{};
    i32 native_status{};
    bool ok() const noexcept {
        return error == AacDecodeError::none;
    }
};
struct AacDecodedBlock {
    AacDecodeError error{};
    i32 native_status{};
    u32 bytes{}, frames{};
    i64 presentation_time_100ns{};
    bool timestamp_valid{};
    bool ok() const noexcept {
        return error == AacDecodeError::none;
    }
};
struct AacDecoder {
    void* runtime{};
    u8 asc[16]{};
    u32 asc_bytes{}, sample_rate{}, channels{}, thread_id{};
    AacPcmFormat format{};
    i32 native_status{};
    u64 accepted_frames{}, output_frames{};
    bool initialized{}, draining{}, ended{};
    AacNativeStage native_stage{};
    // Advertised PCM16/float32 bits 0/1; configured rate/channels bits 2/3.
    // Diagnostic capability observation only, never implicit format fallback.
    u32 native_output_formats{};
};
// Pure metadata init: copies only <=16 ASC bytes. AAC-LC or signaled
// SBR/PS, output mono/stereo and 1024/2048 PCM frames. No PCE/core dependency or
// 960-frame mode. Rate/channels are post-SBR/PS output, not native input values.
// No COM/MF/allocation/device/thread.
bool aac_decoder_configuration_valid(ConstSlice<u8> asc, u32 rate, u32 channels) noexcept;
// MFT stream requirements are byte alignments; MF aligned-buffer creation uses
// a power-of-two-minus-one mask. Zero/one means no stronger alignment required.
// Reject non-power-of-two or >4096 byte requirements without changing output.
bool aac_decoder_alignment_mask(u32 byte_alignment, u32* mask) noexcept;
bool aac_decoder_output_capacity(u32 minimum_bytes, u32* capacity) noexcept;
AacDecodeResult aac_decoder_initialize(AacDecoder*, ConstSlice<u8> asc, u32 rate, u32 channels,
                                       AacPcmFormat format = AacPcmFormat::float32) noexcept;
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
