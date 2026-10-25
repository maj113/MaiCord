#pragma once

#include "maicord/media/asset_key.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/media/mp3_stream.hpp"
#include "maicord/media/wave_pcm.hpp"
#include "maicord/media/mp4_audio_demux.hpp"

namespace maicord {
enum class EncodedAudioError : u8 {
    none,
    invalid_input,
    malformed,
    unsupported,
    truncated,
    checksum,
    capacity,
    cancelled
};

// One validated format and one borrowed immutable encoded source. The caller
// retains its cache lease or owned response bytes until playback has joined.
struct EncodedAudioMetadata {
    MediaAttachmentFormat format{MediaAttachmentFormat::unknown};
    union {
        OggOpusDemuxer opus;
        Mp3Stream mp3;
        WavePcmSource wave;
        Mp4AudioDemuxer m4a;
    };
    constexpr EncodedAudioMetadata() noexcept : opus{} {
    }
    explicit constexpr EncodedAudioMetadata(OggOpusDemuxer source) noexcept
        : format(MediaAttachmentFormat::ogg_opus), opus(source) {
    }
    explicit constexpr EncodedAudioMetadata(Mp3Stream source) noexcept
        : format(MediaAttachmentFormat::mp3), mp3(source) {
    }
    explicit constexpr EncodedAudioMetadata(WavePcmSource source) noexcept
        : format(MediaAttachmentFormat::wav), wave(source) {
    }
    explicit constexpr EncodedAudioMetadata(Mp4AudioDemuxer source) noexcept
        : format(MediaAttachmentFormat::m4a), m4a(source) {
    }
};
inline bool encoded_audio_ready(const EncodedAudioMetadata& source) noexcept {
    switch (source.format) {
    case MediaAttachmentFormat::ogg_opus:
        return source.opus.ready;
    case MediaAttachmentFormat::mp3:
        return source.mp3.ready;
    case MediaAttachmentFormat::wav:
        return source.wave.ready;
    case MediaAttachmentFormat::m4a:
        return source.m4a.samples.ready && source.m4a.sample_rate && source.m4a.channels;
    default:
        return false;
    }
}
inline ConstSlice<u8> encoded_audio_bytes(const EncodedAudioMetadata& source) noexcept {
    switch (source.format) {
    case MediaAttachmentFormat::ogg_opus:
        return source.opus.encoded;
    case MediaAttachmentFormat::mp3:
        return source.mp3.encoded;
    case MediaAttachmentFormat::wav:
        return source.wave.encoded;
    case MediaAttachmentFormat::m4a:
        return source.m4a.samples.encoded;
    default:
        return {};
    }
}
inline u32 encoded_audio_channels(const EncodedAudioMetadata& source) noexcept {
    switch (source.format) {
    case MediaAttachmentFormat::ogg_opus:
        return source.opus.channels;
    case MediaAttachmentFormat::mp3:
        return source.mp3.channels;
    case MediaAttachmentFormat::wav:
        return source.wave.channels;
    case MediaAttachmentFormat::m4a:
        return source.m4a.channels;
    default:
        return 0;
    }
}
inline u32 encoded_audio_rate(const EncodedAudioMetadata& source) noexcept {
    switch (source.format) {
    case MediaAttachmentFormat::ogg_opus:
        return kOggOpusSampleRate;
    case MediaAttachmentFormat::mp3:
        return source.mp3.sample_rate;
    case MediaAttachmentFormat::wav:
        return source.wave.sample_rate;
    case MediaAttachmentFormat::m4a:
        return source.m4a.sample_rate;
    default:
        return 0;
    }
}
inline u64 encoded_audio_total_frames(const EncodedAudioMetadata& source) noexcept {
    switch (source.format) {
    case MediaAttachmentFormat::ogg_opus:
        return source.opus.total_frames;
    case MediaAttachmentFormat::mp3:
        return source.mp3.total_frames;
    case MediaAttachmentFormat::wav:
        return source.wave.total_frames;
    case MediaAttachmentFormat::m4a: {
        // Presented PCM frames after edits, including any leading empty edit.
        Mp4AudioSampleWindow window{};
        return mp4_audio_demux_sample_window(source.m4a, 0, &window) &&
                       window.end_source_frame >= window.first_source_frame
                   ? window.end_source_frame - window.first_source_frame + window.leading_silence_frames
                   : 0;
    }
    default:
        return 0;
    }
}
inline u64 encoded_audio_duration_units(const EncodedAudioMetadata& source, u32 units) noexcept {
    const u64 rate = encoded_audio_rate(source), frames = encoded_audio_total_frames(source);
    if (!encoded_audio_ready(source) || !rate || !units)
        return 0;
    const u64 seconds = frames / rate;
    if (seconds > ~u64{0} / units)
        return ~u64{0};
    const u64 whole = seconds * units, fraction = (frames % rate) * units / rate;
    return fraction > ~u64{0} - whole ? ~u64{0} : whole + fraction;
}
inline u64 encoded_audio_duration_us(const EncodedAudioMetadata& source) noexcept {
    return encoded_audio_duration_units(source, 1'000'000);
}
// Rebind only to a same-size immutable copy of this validated source. WAV data
// remains the exact subrange. Any malformed range leaves all metadata unchanged.
__declspec(noinline) inline bool encoded_audio_rebind(EncodedAudioMetadata* source,
                                                      ConstSlice<u8> bytes) noexcept {
    if (!source || !encoded_audio_ready(*source))
        return false;
    const auto encoded = encoded_audio_bytes(*source);
    if (!encoded.data || encoded.count > ~usize{0} - reinterpret_cast<usize>(encoded.data) || !bytes.data ||
        bytes.count != encoded.count || !bytes.count ||
        bytes.count > ~usize{0} - reinterpret_cast<usize>(bytes.data))
        return false;
    switch (source->format) {
    case MediaAttachmentFormat::ogg_opus:
        source->opus.encoded = bytes;
        return true;
    case MediaAttachmentFormat::m4a:
        // Sample tables are offsets into the same-size copy.
        source->m4a.samples.encoded = bytes;
        return true;
    case MediaAttachmentFormat::mp3:
        source->mp3.encoded = bytes;
        return true;
    case MediaAttachmentFormat::wav: {
        const auto old = source->wave.encoded, data = source->wave.data;
        const usize start = reinterpret_cast<usize>(old.data), address = reinterpret_cast<usize>(data.data);
        if (!old.data || !data.data || address < start || address - start > old.count ||
            data.count > old.count - (address - start))
            return false;
        const u32 offset = static_cast<u32>(address - start);
        source->wave.encoded = bytes;
        source->wave.data = {bytes.data + offset, data.count};
        return true;
    }
    default:
        return false;
    }
}
} // namespace maicord
