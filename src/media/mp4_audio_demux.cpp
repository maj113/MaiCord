#include "maicord/media/mp4_audio_demux.hpp"
#include "maicord/media/aac_configuration.hpp"
#include "mp4_demux_detail.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
using namespace mp4_demux_detail;
struct Descriptor {
    u8 tag;
    u64 payload, end;
};
bool descriptor(ConstSlice<u8> source, u64* cursor, u64 end, Descriptor* output) noexcept {
    if (*cursor >= end || end > source.count)
        return false;
    // cursor < end <= source.count (a u32), so the narrowing below cannot truncate.
    const u8 tag = source[static_cast<u32>((*cursor)++)];
    u32 size{};
    bool terminal = false;
    for (u32 i = 0; i < 4; ++i) {
        if (*cursor >= end)
            return false;
        const u8 b = source[static_cast<u32>((*cursor)++)];
        size = (size << 7) | (b & 127);
        if (!(b & 128)) {
            terminal = true;
            break;
        }
    }
    if (!terminal || size > end - *cursor)
        return false;
    *output = {tag, *cursor, *cursor + size};
    *cursor += size;
    return true;
}
// Exactly one esds, directly in the sample entry or (QuickTime) in its 'wave'
// atom, whose frma/terminator children are ordinary sized boxes. Either list
// may also close with a bare 32-bit zero.
bool scan_audio_extensions(ConstSlice<u8> source, u64 cursor, u64 end, Box* esds, bool nested = false) noexcept {
    while (!atom_list_end(source, cursor, end)) {
        Box extension{};
        if (!next_box(source, &cursor, end, &extension))
            return false;
        if (extension.type == fourcc('e', 's', 'd', 's')) {
            if (esds->end)
                return false;
            *esds = extension;
        } else if (!nested && extension.type == fourcc('w', 'a', 'v', 'e') &&
                   !scan_audio_extensions(source, extension.payload, extension.end, esds, true)) {
            return false;
        }
    }
    return true;
}
Mp4DemuxError description(Mp4AudioDemuxer& d, const Box& stsd) noexcept {
    const auto source = d.samples.encoded;
    AacConfiguration configuration{};
    if (stsd.end - stsd.payload < 8 || read_u32(source.data + stsd.payload) != 0)
        return Mp4DemuxError::malformed;
    if (!read_u32(source.data + stsd.payload + 4))
        return Mp4DemuxError::unsupported;
    u64 cursor{};
    Box entry{};
    if (!first_description(source, stsd, &entry))
        return Mp4DemuxError::malformed;
    if (entry.type != fourcc('m', 'p', '4', 'a'))
        return Mp4DemuxError::unsupported;
    if (entry.end - entry.payload < 28)
        return Mp4DemuxError::malformed;
    const auto* p = source.data + entry.payload;
    // ISO entries use QuickTime SoundDescription version 0. QuickTime movies
    // also write version 1 (four extra fields) and version 2 (rate/channels
    // moved into a 36-byte extension); the codec configuration is unchanged.
    const u32 version = (u32{p[8]} << 8) | p[9];
    if (p[6] != 0 || p[7] != 1 || version > 2)
        return Mp4DemuxError::unsupported;
    const u32 header = version == 0 ? 28 : version == 1 ? 44 : 64;
    if (entry.end - entry.payload < header)
        return Mp4DemuxError::malformed;
    u32 channels = (u32{p[16]} << 8) | p[17];
    u32 rate = read_u32(p + 24);
    if (version == 2) {
        const u64 bits = read_u64(p + 32);
        double sample_rate{};
        copy_bytes(&sample_rate, &bits, sizeof(sample_rate));
        channels = read_u32(p + 40);
        if (!(sample_rate >= 8000.0 && sample_rate <= 96000.0) ||
            static_cast<double>(static_cast<u32>(sample_rate)) != sample_rate)
            return Mp4DemuxError::unsupported;
        rate = static_cast<u32>(sample_rate) << 16;
    }
    if ((channels != 1 && channels != 2) || (rate & 65535))
        return Mp4DemuxError::unsupported;
    Box esds{};
    if (!scan_audio_extensions(source, entry.payload + header, entry.end, &esds))
        return Mp4DemuxError::malformed;
    if (!esds.end || esds.end - esds.payload < 4 || read_u32(source.data + esds.payload) != 0)
        return Mp4DemuxError::malformed;
    cursor = esds.payload + 4;
    Descriptor es{};
    if (!descriptor(source, &cursor, esds.end, &es) || es.tag != 3 || cursor != esds.end ||
        es.end - es.payload < 3)
        return Mp4DemuxError::malformed;
    if (source[static_cast<u32>(es.payload + 2)] & 0xe0)
        return Mp4DemuxError::unsupported;
    cursor = es.payload + 3;
    Descriptor config{};
    if (!descriptor(source, &cursor, es.end, &config) || config.tag != 4 ||
        config.end - config.payload < 13)
        return Mp4DemuxError::malformed;
    // MPEG-4 audio (0x40) in an audio stream (type 5). The low upStream and
    // reserved bits vary by muxer: Apple writes 0x14 where ISO muxers write 0x15.
    if (source[static_cast<u32>(config.payload)] != 0x40 ||
        (source[static_cast<u32>(config.payload + 1)] & 0xfc) != 0x14)
        return Mp4DemuxError::unsupported;
    u64 decoder_cursor = config.payload + 13;
    bool found = false;
    while (decoder_cursor < config.end) {
        Descriptor child{};
        if (!descriptor(source, &decoder_cursor, config.end, &child))
            return Mp4DemuxError::malformed;
        if (child.tag == 5) {
            if (found)
                return Mp4DemuxError::malformed;
            found = true;
            if (child.end - child.payload > 16 ||
                !aac_configuration_decode(
                    {source.data + child.payload, static_cast<u32>(child.end - child.payload)},
                    &configuration))
                return Mp4DemuxError::unsupported;
            d.sample_rate = configuration.sample_rate;
            d.channels = configuration.channels;
            d.frames_per_packet = configuration.frames_per_packet;
            d.asc_offset = static_cast<u32>(child.payload);
            d.asc_bytes = static_cast<u32>(child.end - child.payload);
        }
    }
    if (!found)
        return Mp4DemuxError::malformed;
    while (cursor < es.end) {
        Descriptor child{};
        if (!descriptor(source, &cursor, es.end, &child))
            return Mp4DemuxError::malformed;
        if (child.tag != 6 || child.end - child.payload != 1 ||
            source[static_cast<u32>(child.payload)] != 2)
            return Mp4DemuxError::unsupported;
    }
    if ((channels != configuration.core_channels && channels != configuration.channels) ||
        ((rate >> 16) != configuration.core_sample_rate && (rate >> 16) != configuration.sample_rate))
        return Mp4DemuxError::unsupported;
    // The ASC fixes the PCM domain: LC 1024 or doubled SBR 2048. Muxers such
    // as ffmpeg give the final packet only the audio's real length, so a last
    // run of one packet may be shorter; the sample window trims its padding.
    const u64 packet = u64{d.samples.timescale} * d.frames_per_packet;
    for (u32 i = 0; i < d.samples.stts_count; ++i) {
        const u8* run = source.data + d.samples.stts_offset + u64{i} * 8;
        const u64 length = u64{read_u32(run + 4)} * d.sample_rate;
        const bool final_packet = i + 1 == d.samples.stts_count && read_u32(run) == 1;
        if (length != packet && !(final_packet && length < packet))
            return Mp4DemuxError::unsupported;
    }
    // A validated redundant all-zero CTTS table leaves CT=DT unchanged. The
    // Windows MP4 sink emits this even for LC audio without reordering. Accept
    // only that identity mapping, never infer a nonzero encoder delay.
    // https://developer.apple.com/documentation/quicktime-file-format/composition_offset_atom
    for (u32 i = 0; i < d.samples.ctts_count; ++i)
        if (read_u32(source.data + d.samples.ctts_offset + u64{i} * 8 + 4))
            return Mp4DemuxError::unsupported;
    return Mp4DemuxError::none;
}
} // namespace
Mp4DemuxError mp4_audio_demux_initialize(Mp4AudioDemuxer* output, ConstSlice<u8> source,
                                         Mp4SourceAvailability availability) noexcept {
    if (!output || !byte_range_valid({reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        !byte_range_valid(source) ||
        byte_ranges_overlap(source, {reinterpret_cast<const u8*>(output), sizeof(*output)}))
        return Mp4DemuxError::invalid_input;
    *output = {};
    Mp4AudioDemuxer d{};
    mp4_demux_detail::Box stsd{};
    auto error = mp4_demux_detail::initialize_tables(&d.samples, source,
                                                     mp4_demux_detail::fourcc('s', 'o', 'u', 'n'), &stsd,
                                                     availability);
    if (error == Mp4DemuxError::none)
        error = description(d, stsd);
    if (error == Mp4DemuxError::none)
        *output = d;
    return error;
}
ConstSlice<u8> mp4_audio_demux_asc(const Mp4AudioDemuxer& d) noexcept {
    if (!d.samples.ready || !d.asc_bytes || d.asc_offset > d.samples.encoded.count ||
        d.asc_bytes > d.samples.encoded.count - d.asc_offset)
        return {};
    return {d.samples.encoded.data + d.asc_offset, d.asc_bytes};
}
Mp4AccessUnit mp4_audio_demux_next(Mp4AudioDemuxer* d) noexcept {
    return mp4_demux_next(d ? &d->samples : nullptr);
}
void mp4_audio_demux_reset(Mp4AudioDemuxer* d) noexcept {
    if (d)
        mp4_demux_reset(&d->samples);
}
namespace {
constexpr i64 signed_limit = 0x7fffffffffffffffll;
struct Time {
    i64 seconds{};
    u64 numerator{}, denominator{1};
};
u64 gcd(u64 a, u64 b) noexcept {
    while (b) {
        const u64 remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}
bool positive_time(u64 ticks, u32 scale, Time* out) noexcept {
    if (!scale || ticks / scale > static_cast<u64>(signed_limit))
        return false;
    const u64 remainder = ticks % scale, reduction = gcd(remainder, scale);
    *out = {static_cast<i64>(ticks / scale), remainder / reduction, u64{scale} / reduction};
    return true;
}
bool add_time(const Time& a, const Time& b, Time* out) noexcept {
    if (!a.denominator || !b.denominator || a.numerator >= a.denominator || b.numerator >= b.denominator ||
        (b.seconds > 0 && a.seconds > signed_limit - b.seconds) ||
        (b.seconds < 0 && a.seconds < (-signed_limit - 1) - b.seconds))
        return false;
    const u64 common = gcd(a.denominator, b.denominator), factor = b.denominator / common;
    if (a.denominator > ~u64{0} / factor)
        return false;
    const u64 denominator = a.denominator * factor;
    const u64 first = a.numerator * factor, second = b.numerator * (a.denominator / common);
    const bool carry = first >= denominator - second;
    const u64 numerator = carry ? first - (denominator - second) : first + second;
    i64 seconds = a.seconds + b.seconds;
    if (carry) {
        if (seconds == signed_limit)
            return false;
        ++seconds;
    }
    const u64 reduction = gcd(numerator, denominator);
    *out = {seconds, numerator / reduction, denominator / reduction};
    return true;
}
Time negate_time(const Time& positive) noexcept {
    return positive.numerator ? Time{-positive.seconds - 1, positive.denominator - positive.numerator,
                                     positive.denominator}
                              : Time{-positive.seconds, 0, 1};
}
// Scale a proper fraction without a compiler-specific 128-bit type. Common
// timebases use the one-division fast path; extreme denominators use 32
// modular double/add steps, each proven not to overflow u64.
void fraction_scale(u64 numerator, u64 denominator, u32 scale, u64* quotient, u64* remainder) noexcept {
    if (numerator <= ~u64{0} / scale) {
        const u64 product = numerator * scale;
        *quotient = product / denominator;
        *remainder = product % denominator;
        return;
    }
    u64 value{}, rest{};
    for (u32 n = 32; n; --n) {
        const bool twice = rest >= denominator - rest;
        rest = twice ? rest - (denominator - rest) : rest + rest;
        value = value * 2 + u64{twice};
        if ((scale >> (n - 1)) & 1) {
            const bool plus = rest >= denominator - numerator;
            rest = plus ? rest - (denominator - numerator) : rest + numerator;
            value += u64{plus};
        }
    }
    *quotient = value;
    *remainder = rest;
}
bool native_time(const Time& time, i64* out, bool* fractional = nullptr) noexcept {
    u64 fraction{}, remainder{};
    fraction_scale(time.numerator, time.denominator, 10000000, &fraction, &remainder);
    const u64 magnitude =
        time.seconds < 0 ? static_cast<u64>(-(time.seconds + 1)) + 1 : static_cast<u64>(time.seconds);
    if (magnitude > ~u64{0} / 10000000)
        return false;
    const u64 base = magnitude * 10000000;
    if (time.seconds >= 0) {
        if (base > static_cast<u64>(signed_limit) - fraction)
            return false;
        *out = static_cast<i64>(base + fraction);
    } else {
        const u64 negative = base - fraction;
        if (negative > (u64{1} << 63))
            return false;
        *out = negative == (u64{1} << 63) ? -signed_limit - 1 : -static_cast<i64>(negative);
    }
    if (fractional)
        *fractional = remainder != 0;
    return true;
}
bool frame_ceil(const Time& time, u32 rate, u64* out) noexcept {
    if (time.seconds < 0) {
        *out = 0;
        return true;
    }
    const u64 seconds = static_cast<u64>(time.seconds);
    if (seconds > ~u64{0} / rate)
        return false;
    u64 fraction{}, remainder{};
    fraction_scale(time.numerator, time.denominator, rate, &fraction, &remainder);
    fraction += u64{remainder != 0};
    const u64 base = seconds * rate;
    if (base > ~u64{0} - fraction)
        return false;
    *out = base + fraction;
    return true;
}
bool timing(const Mp4AudioDemuxer& d, Time* start, Time* empty, Time* duration) noexcept {
    const auto& s = d.samples;
    return s.ready && (d.frames_per_packet == 1024 || d.frames_per_packet == 2048) &&
           d.sample_rate >= 8000 && d.sample_rate <= 48000 && s.sample_count && s.media_start_ticks >= 0 &&
           s.movie_duration_ticks >= s.empty_edit_ticks &&
           positive_time(static_cast<u64>(s.media_start_ticks), s.timescale, start) &&
           positive_time(s.empty_edit_ticks, s.movie_timescale, empty) &&
           positive_time(s.movie_duration_ticks, s.movie_timescale, duration);
}
bool frame_time(const Mp4AudioDemuxer& d, u64 frame, const Time& start, const Time& empty,
                Time* out) noexcept {
    Time source{}, relative{};
    return frame <= u64{d.samples.sample_count} * d.frames_per_packet &&
           positive_time(frame, d.sample_rate, &source) &&
           add_time(source, negate_time(start), &relative) && add_time(relative, empty, out);
}
bool output_valid(const Mp4AudioDemuxer& d, const void* output, u32 bytes) noexcept {
    const ConstSlice<u8> range{static_cast<const u8*>(output), bytes};
    return output && byte_range_valid(range) &&
           !byte_ranges_overlap(range, {reinterpret_cast<const u8*>(&d), sizeof(d)}) &&
           !byte_ranges_overlap(range, d.samples.encoded);
}
} // namespace
bool mp4_audio_demux_frame_time_100ns(const Mp4AudioDemuxer& d, u64 frame, i64* output) noexcept {
    if (!output_valid(d, output, sizeof(*output)))
        return false;
    Time start{}, empty{}, duration{}, mapped{};
    i64 value{};
    if (!timing(d, &start, &empty, &duration) || !frame_time(d, frame, start, empty, &mapped) ||
        !native_time(mapped, &value))
        return false;
    *output = value;
    return true;
}
bool mp4_audio_demux_frame_time_bounds_100ns(const Mp4AudioDemuxer& d, u64 frame,
                                             Mp4AudioFrameTimeBounds* output) noexcept {
    if (!output_valid(d, output, sizeof(*output)))
        return false;
    Time start{}, empty{}, duration{}, mapped{};
    Mp4AudioFrameTimeBounds bounds{};
    bool fractional{};
    if (!timing(d, &start, &empty, &duration) || !frame_time(d, frame, start, empty, &mapped) ||
        !native_time(mapped, &bounds.floor_100ns, &fractional) ||
        (fractional && bounds.floor_100ns == signed_limit))
        return false;
    bounds.ceil_100ns = bounds.floor_100ns + i64{fractional};
    *output = bounds;
    return true;
}
bool mp4_audio_demux_sample_window(const Mp4AudioDemuxer& d, i64 target,
                                   Mp4AudioSampleWindow* output) noexcept {
    if (target < 0 || !output_valid(d, output, sizeof(*output)))
        return false;
    Time start{}, empty{}, duration{};
    Mp4AudioSampleWindow window{};
    if (!timing(d, &start, &empty, &duration) || !native_time(duration, &window.duration_100ns) ||
        target > window.duration_100ns)
        return false;
    const u64 total = u64{d.samples.sample_count} * d.frames_per_packet;
    Time requested{}, relative{}, inverse{}, span{}, source_end{};
    if (!positive_time(static_cast<u64>(target), 10000000, &requested) ||
        !add_time(requested, negate_time(empty), &relative) || !add_time(relative, start, &inverse))
        return false;
    u64 first{}, initial{}, end = total;
    if (!frame_ceil(inverse, d.sample_rate, &first) || !frame_ceil(start, d.sample_rate, &initial))
        return false;
    if (d.samples.has_edit_list) {
        if (!positive_time(d.samples.movie_duration_ticks - d.samples.empty_edit_ticks,
                           d.samples.movie_timescale, &span) ||
            !add_time(start, span, &source_end) || !frame_ceil(source_end, d.sample_rate, &end))
            return false;
    } else if (!frame_ceil(duration, d.sample_rate, &end)) {
        // Without edits the track's length ends it, short final packet included.
        return false;
    }
    window.end_source_frame = minimum(end, total);
    window.first_source_frame = minimum(maximum(first, initial), window.end_source_frame);
    if (window.first_source_frame < window.end_source_frame) {
        Time first_time{}, gap{};
        if (!frame_time(d, window.first_source_frame, start, empty, &first_time) ||
            !add_time(first_time, negate_time(requested), &gap) ||
            !frame_ceil(gap, d.sample_rate, &window.leading_silence_frames))
            return false;
    }
    *output = window;
    return true;
}
} // namespace maicord
