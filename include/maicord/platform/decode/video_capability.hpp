#pragma once

#include "maicord/platform/decode/gpu_stream_decoder.hpp"

namespace maicord {

// Which decoders a machine has for live video, and what the client therefore tells Discord. The Linux backends
// probe once (VA-API through the render node of the GPU the renderer uses, then the dlopen()ed software libraries)
// and hand the two results here; the resolution is pure so tests can feed it any combination.
//
// Hardware wins whenever it can decode a codec. A codec only the software tier decodes is still advertised (H.264
// through libopenh264 keeps mixed rooms working, AV1 through libdav1d), but marked, so that the offer lists the
// hardware codecs first: Discord's priority is the order of the offer, and a room should only fall to a software
// codec when no hardware one is common. HEVC has no software tier and is advertised only when the hardware decodes it.
enum class VideoDecodeTier : u8 { none, software, hardware };

// Bit sets of gpu_stream_codec_bit values.
struct VideoDecodeProbe {
    u32 hardware;
    u32 software;
};

inline constexpr u32 kVideoDecodeCodecMask = 0x7;
// The codecs the software tier can decode at all (HEVC is not among them).
inline constexpr u32 kVideoDecodeSoftwareCapable =
    (1u << static_cast<u8>(GpuStreamCodec::h264)) | (1u << static_cast<u8>(GpuStreamCodec::av1));

constexpr VideoDecodeTier video_decode_tier(const VideoDecodeProbe& probe, GpuStreamCodec codec) noexcept {
    const u32 bit = gpu_stream_codec_bit(codec);
    if (probe.hardware & bit)
        return VideoDecodeTier::hardware;
    if ((probe.software & bit) && (kVideoDecodeSoftwareCapable & bit))
        return VideoDecodeTier::software;
    return VideoDecodeTier::none;
}

// The codecs advertised: every codec with a tier.
constexpr u32 video_decode_advertised(const VideoDecodeProbe& probe) noexcept {
    u32 codecs = 0;
    for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec)
        if (video_decode_tier(probe, static_cast<GpuStreamCodec>(codec)) != VideoDecodeTier::none)
            codecs |= 1u << codec;
    return codecs;
}

// The advertised codecs only the software tier decodes.
constexpr u32 video_decode_software_only(const VideoDecodeProbe& probe) noexcept {
    u32 codecs = 0;
    for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec)
        if (video_decode_tier(probe, static_cast<GpuStreamCodec>(codec)) == VideoDecodeTier::software)
            codecs |= 1u << codec;
    return codecs;
}

// The value of gpu_stream_decoder_codecs on Linux: the advertised codecs in bits 0-2 and, in bits 8-10, the subset
// only the software tier decodes. (A Windows device reports the first byte only, hardware by construction.)
inline constexpr u32 kGpuStreamSoftwareShift = 8;
constexpr u32 video_decode_codecs_word(const VideoDecodeProbe& probe) noexcept {
    return video_decode_advertised(probe) | (video_decode_software_only(probe) << kGpuStreamSoftwareShift);
}
constexpr u32 gpu_stream_codecs_decodable(u32 word) noexcept {
    return word & kVideoDecodeCodecMask;
}
constexpr u32 gpu_stream_codecs_software_only(u32 word) noexcept {
    return (word >> kGpuStreamSoftwareShift) & kVideoDecodeCodecMask;
}

} // namespace maicord
