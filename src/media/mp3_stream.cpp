#include "maicord/media/mp3_stream.hpp"
#include "maicord/media/mp3_decoder.h"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
constexpr u32 maximum_bytes = 64u * 1024 * 1024;
static_assert(static_cast<int>(Mp3Error::malformed) == 2 && static_cast<int>(Mp3Error::unsupported) == 3 &&
              static_cast<int>(Mp3Error::truncated) == 4);
bool cancelled(Mp3Cancelled callback, void* context) noexcept {
    return callback && callback(context);
}
bool tag(const u8* p, const char* text, u32 bytes) noexcept {
    return bytes_equal(p, text, bytes);
}
// Header and side information, bounded as the decoder consumes them. The
// protection CRC is skipped, not verified. Callers pass offsets within bytes.
Mp3Error frame(ConstSlice<u8> bytes, u32 offset, Mp3Frame* out) noexcept {
    return static_cast<Mp3Error>(mp3_frame_parse(bytes.data + offset, bytes.count - offset, out));
}
bool format(const Mp3Frame& f, const Mp3Stream& s) noexcept {
    return f.sample_rate == s.sample_rate && f.channels == s.channels && f.version == s.mpeg_version &&
           f.samples == s.frames_per_packet;
}
bool current(const Mp3Stream& s) noexcept {
    return s.ready && s.encoded.data && s.encoded.count <= maximum_bytes && s.sample_rate &&
           (s.channels == 1 || s.channels == 2) && s.audio_begin < s.audio_end &&
           s.audio_end <= s.encoded.count && s.frame_count &&
           s.raw_frames == static_cast<u64>(s.frame_count) * s.frames_per_packet &&
           static_cast<u64>(s.delay_frames) + s.padding_frames < s.raw_frames &&
           s.total_frames == s.raw_frames - s.delay_frames - s.padding_frames;
}
Mp3Error tags(ConstSlice<u8> bytes, u32* begin, u32* end) noexcept {
    u32 first = 0, last = bytes.count;
    if (last >= 3 && tag(bytes.data, "ID3", 3)) {
        if (last < 10)
            return Mp3Error::truncated;
        const u8* h = bytes.data;
        if (h[3] < 2 || h[3] > 4 || h[4] == 255 ||
            (h[5] & (h[3] == 2   ? 0x3f
                     : h[3] == 3 ? 0x1f
                                 : 0x0f)) ||
            ((h[6] | h[7] | h[8] | h[9]) & 0x80))
            return Mp3Error::malformed;
        const u32 payload = (static_cast<u32>(h[6]) << 21) | (static_cast<u32>(h[7]) << 14) |
                            (static_cast<u32>(h[8]) << 7) | h[9];
        const u32 footer = h[3] == 4 && (h[5] & 0x10) ? 10u : 0u;
        const u64 size = static_cast<u64>(payload) + 10 + footer;
        if (size > last)
            return Mp3Error::truncated;
        first = static_cast<u32>(size);
        if (footer && (!tag(h + first - 10, "3DI", 3) || !bytes_equal(h + 3, h + first - 7, 7)))
            return Mp3Error::malformed;
    }
    if (last - first >= 128 && tag(bytes.data + last - 128, "TAG", 3)) {
        last -= 128;
        if (last - first >= 227 && tag(bytes.data + last - 227, "TAG+", 4))
            last -= 227;
    }
    if (last - first >= 32 && tag(bytes.data + last - 32, "APETAGEX", 8)) {
        const u8* footer = bytes.data + last - 32;
        const u32 version = byte_load_le32(footer + 8), size = byte_load_le32(footer + 12),
                  flags = byte_load_le32(footer + 20);
        if ((version != 1000 && version != 2000) || size < 32 || size > last - first ||
            (flags & 0x60000000u) || (flags & 0x1ffffff8u))
            return Mp3Error::malformed;
        for (u32 i = 24; i < 32; ++i)
            if (footer[i])
                return Mp3Error::malformed;
        last -= size;
        if (flags & 0x80000000u) {
            if (last - first < 32 || !tag(bytes.data + last - 32, "APETAGEX", 8))
                return Mp3Error::malformed;
            const u8* header = bytes.data + last - 32;
            if (byte_load_le32(header + 8) != version || byte_load_le32(header + 12) != size ||
                byte_load_le32(header + 16) != byte_load_le32(footer + 16) ||
                byte_load_le32(header + 20) != (flags | 0x20000000u))
                return Mp3Error::malformed;
            last -= 32;
        }
    }
    if (first >= last)
        return Mp3Error::truncated;
    *begin = first;
    *end = last;
    return Mp3Error::none;
}
struct Vbr {
    u32 frames{}, bytes{}, delay{}, padding{};
    bool header{}, gapless{};
};
Mp3Error vbr(ConstSlice<u8> bytes, u32 begin, const Mp3Frame& f, Vbr* out) noexcept {
    const u8* h = bytes.data + begin;
    Vbr info{};
    u32 offset = f.bytes - f.main_bytes;
    if (f.bytes - offset >= 4 && (tag(h + offset, "Xing", 4) || tag(h + offset, "Info", 4))) {
        info.header = true;
        if (f.bytes - offset < 8)
            return Mp3Error::truncated;
        const u32 flags = byte_load_be32(h + offset + 4);
        if (flags & ~15u)
            return Mp3Error::malformed;
        offset += 8;
        const u32 fields[]{4, 4, 100, 4};
        for (u32 bit = 0; bit < 4; ++bit)
            if (flags & (1u << bit)) {
                if (fields[bit] > f.bytes - offset)
                    return Mp3Error::truncated;
                if (!bit)
                    info.frames = byte_load_be32(h + offset);
                if (bit == 1)
                    info.bytes = byte_load_be32(h + offset);
                offset += fields[bit];
            }
        if ((flags & 1) && !info.frames)
            return Mp3Error::malformed;
        if ((flags & 2) && !info.bytes)
            return Mp3Error::malformed;
        if (f.bytes - offset >= 4 &&
            (tag(h + offset, "LAME", 4) || tag(h + offset, "Lavc", 4) || tag(h + offset, "Lavf", 4))) {
            if (f.bytes - offset < 36)
                return Mp3Error::truncated;
            const u8* trim = h + offset + 21;
            const u32 delay = (static_cast<u32>(trim[0]) << 4) | (trim[1] >> 4);
            const u32 padding = (static_cast<u32>(trim[1] & 15) << 8) | trim[2];
            // No missing decoded tail can be recovered for a tag with insufficient
            // encoder padding; reject rather than shorten/invent audible PCM.
            if (padding < 529)
                return Mp3Error::unsupported;
            info.delay = delay + 529;
            info.padding = padding - 529;
            info.gapless = true;
        }
    } else if (f.bytes >= 54 && tag(h + 36, "VBRI", 4)) {
        if (h[40] || h[41] != 1)
            return Mp3Error::unsupported;
        info.header = true;
        info.bytes = byte_load_be32(h + 46);
        info.frames = byte_load_be32(h + 50);
        if (!info.bytes || !info.frames)
            return Mp3Error::malformed;
    }
    *out = info;
    return Mp3Error::none;
}
bool decoder_current(const Mp3Decoder* d) noexcept {
    return d && d->initialized && d->state && d->state_bytes == sizeof(Mp3DecoderState) &&
           !(reinterpret_cast<usize>(d->state) & (kMp3DecoderStateAlignment - 1)) &&
           (d->channels == 1 || d->channels == 2) && d->sample_rate;
}
} // namespace

Mp3Error mp3_stream_initialize(Mp3Stream* output, ConstSlice<u8> bytes, Mp3Cancelled cancel,
                               void* context) noexcept {
    if (!output || !bytes.data || !bytes.count || bytes.count > maximum_bytes ||
        byte_ranges_overlap(output, sizeof(*output), bytes.data, bytes.count))
        return Mp3Error::invalid_input;
    if (cancelled(cancel, context))
        return Mp3Error::cancelled;
    Mp3Stream s{};
    s.encoded = bytes;
    Mp3Error error = tags(bytes, &s.audio_begin, &s.audio_end);
    if (error != Mp3Error::none)
        return error;
    Mp3Frame first{};
    error = frame({bytes.data, s.audio_end}, s.audio_begin, &first);
    if (error != Mp3Error::none)
        return error;
    s.sample_rate = first.sample_rate;
    s.channels = first.channels;
    s.mpeg_version = first.version;
    s.frames_per_packet = first.samples;
    Vbr metadata{};
    error = vbr(bytes, s.audio_begin, first, &metadata);
    if (error != Mp3Error::none)
        return error;
    const u32 encoded_begin = s.audio_begin;
    if (metadata.header)
        s.audio_begin += first.bytes;
    u32 offset = s.audio_begin;
    u64 main_bytes = 0;
    while (offset < s.audio_end) {
        if (cancelled(cancel, context))
            return Mp3Error::cancelled;
        Mp3Frame f{};
        error = frame({bytes.data, s.audio_end}, offset, &f);
        if (error != Mp3Error::none)
            return error;
        if (!format(f, s))
            return Mp3Error::unsupported;
        if (f.main_data_begin > main_bytes)
            return Mp3Error::malformed;
        main_bytes += f.main_bytes;
        ++s.frame_count;
        offset += f.bytes;
    }
    if (!s.frame_count || (metadata.frames && metadata.frames != s.frame_count) ||
        (metadata.bytes && metadata.bytes != s.audio_end - encoded_begin))
        return Mp3Error::malformed;
    s.raw_frames = static_cast<u64>(s.frame_count) * s.frames_per_packet;
    s.delay_frames = metadata.delay;
    s.padding_frames = metadata.padding;
    s.gapless = metadata.gapless;
    if (static_cast<u64>(s.delay_frames) + s.padding_frames >= s.raw_frames)
        return Mp3Error::malformed;
    s.total_frames = s.raw_frames - s.delay_frames - s.padding_frames;
    s.cursor_offset = s.audio_begin;
    s.discard_before = s.delay_frames;
    s.ready = true;
    *output = s;
    return Mp3Error::none;
}
Mp3Packet mp3_stream_next(Mp3Stream* s) noexcept {
    Mp3Packet packet{};
    if (!s || !current(*s) || s->cursor_offset < s->audio_begin || s->cursor_offset > s->audio_end ||
        s->cursor_frame > s->raw_frames || s->cursor_frame % s->frames_per_packet) {
        packet.error = Mp3Error::invalid_input;
        return packet;
    }
    const u64 end = s->raw_frames - s->padding_frames;
    if (s->cursor_offset == s->audio_end || s->cursor_frame >= end) {
        packet.error = Mp3Error::end_of_stream;
        return packet;
    }
    Mp3Frame f{};
    packet.error = frame({s->encoded.data, s->audio_end}, s->cursor_offset, &f);
    if (!packet.ok())
        return packet;
    if (!format(f, *s)) {
        packet.error = Mp3Error::unsupported;
        return packet;
    }
    packet.bytes = {s->encoded.data + s->cursor_offset, f.bytes};
    packet.decoded_first_frame = s->cursor_frame;
    packet.duration_frames = f.samples;
    const u64 front = s->discard_before > s->cursor_frame ? s->discard_before - s->cursor_frame : 0;
    packet.trim_front_frames = static_cast<u32>(front < f.samples ? front : f.samples);
    const u64 after = s->cursor_frame + f.samples;
    packet.trim_back_frames = after > end ? static_cast<u32>(after - end) : 0;
    if (packet.trim_back_frames > f.samples - packet.trim_front_frames)
        packet.trim_back_frames = f.samples - packet.trim_front_frames;
    s->cursor_offset += f.bytes;
    s->cursor_frame = after;
    return packet;
}
Mp3Error mp3_stream_build_index(const Mp3Stream& s, Mp3SeekIndex* output, Slice<Mp3SeekPoint> bank,
                                Mp3Cancelled cancel, void* context) noexcept {
    const usize bank_bytes = static_cast<usize>(bank.count) * sizeof(Mp3SeekPoint);
    if (!current(s) || !output || !bank.data || bank.count < 2 || bank.count > 4096 ||
        byte_ranges_overlap(bank.data, bank_bytes, s.encoded.data, s.encoded.count) ||
        byte_ranges_overlap(output, sizeof(*output), bank.data, bank_bytes))
        return Mp3Error::invalid_input;
    Mp3SeekIndex result{};
    result.points = bank;
    result.source = s.encoded.data;
    result.source_bytes = s.encoded.count;
    result.frame_count = s.frame_count;
    // A cancelled rebuild may have partially overwritten caller checkpoints;
    // withdraw the old index authority before the first write.
    reset_value(output);
    const u32 stride = (s.frame_count - 1) / bank.count + 1;
    u32 offset = s.audio_begin;
    u64 main_bytes = 0;
    for (u32 i = 0; i < s.frame_count; ++i) {
        if (cancelled(cancel, context))
            return Mp3Error::cancelled;
        Mp3Frame f{};
        const Mp3Error error = frame({s.encoded.data, s.audio_end}, offset, &f);
        if (error != Mp3Error::none)
            return error;
        if (!format(f, s))
            return Mp3Error::unsupported;
        if (!(i % stride))
            bank[result.point_count++] = {static_cast<u64>(i) * f.samples, main_bytes, offset, i};
        main_bytes += f.main_bytes;
        offset += f.bytes;
    }
    if (offset != s.audio_end)
        return Mp3Error::malformed;
    *output = result;
    return Mp3Error::none;
}
Mp3Error mp3_stream_seek(Mp3Stream* s, u64 target, const Mp3SeekIndex* index, Mp3Cancelled cancel,
                         void* context) noexcept {
    if (!s || !current(*s) || target > s->total_frames)
        return Mp3Error::invalid_input;
    if (cancelled(cancel, context))
        return Mp3Error::cancelled;
    u32 offset = s->audio_begin;
    u64 first = 0;
    if (index) {
        if (index->source != s->encoded.data || index->source_bytes != s->encoded.count ||
            index->frame_count != s->frame_count || !index->points.data || !index->point_count ||
            index->point_count > index->points.count || index->point_count > 4096)
            return Mp3Error::invalid_input;
        const auto& zero = index->points[0];
        if (zero.offset != s->audio_begin || zero.frame_index || zero.first_frame || zero.main_data_bytes)
            return Mp3Error::invalid_input;
        for (u32 i = 0; i < index->point_count; ++i) {
            if (cancelled(cancel, context))
                return Mp3Error::cancelled;
            const auto& point = index->points[i];
            if (point.offset < s->audio_begin || point.offset >= s->audio_end ||
                point.frame_index >= s->frame_count ||
                point.first_frame != static_cast<u64>(point.frame_index) * s->frames_per_packet ||
                (i && (point.offset <= index->points[i - 1].offset ||
                       point.frame_index <= index->points[i - 1].frame_index ||
                       point.main_data_bytes < index->points[i - 1].main_data_bytes)))
                return Mp3Error::invalid_input;
        }
        const u64 raw = target + s->delay_frames;
        const u64 dsp_start = raw / s->frames_per_packet > 2 ? raw / s->frames_per_packet - 2 : 0;
        u32 anchor = 0;
        while (anchor + 1 < index->point_count && index->points[anchor + 1].frame_index <= dsp_start)
            ++anchor;
        u32 start = anchor;
        // Fill the reservoir BEFORE the two fully decoded DSP priming frames.
        while (start && index->points[anchor].main_data_bytes - index->points[start].main_data_bytes < 511)
            --start;
        offset = index->points[start].offset;
        first = index->points[start].first_frame;
        Mp3Frame f{};
        const Mp3Error error = frame({s->encoded.data, s->audio_end}, offset, &f);
        if (error != Mp3Error::none)
            return error;
        if (!format(f, *s))
            return Mp3Error::unsupported;
    }
    if (target == s->total_frames) {
        offset = s->audio_end;
        first = s->raw_frames;
    }
    s->cursor_offset = offset;
    s->cursor_frame = first;
    s->discard_before = target + s->delay_frames;
    return Mp3Error::none;
}
u32 mp3_decoder_storage_bytes() noexcept {
    return static_cast<u32>(sizeof(Mp3DecoderState));
}
Mp3Error mp3_decoder_initialize(Mp3Decoder* decoder, Slice<u8> storage, u32 rate, u32 channels) noexcept {
    static constexpr u32 rates[]{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    bool supported = false;
    for (u32 candidate : rates)
        supported = supported || candidate == rate;
    if (!decoder || decoder->state || decoder->initialized || !supported ||
        (channels != 1 && channels != 2) || !storage.data ||
        (reinterpret_cast<usize>(storage.data) & (kMp3DecoderStateAlignment - 1)) ||
        byte_ranges_overlap(decoder, sizeof(*decoder), storage.data, sizeof(Mp3DecoderState)))
        return Mp3Error::invalid_input;
    if (storage.count < sizeof(Mp3DecoderState))
        return Mp3Error::capacity;
    // A zeroed state is the initial one.
    clear_bytes(storage.data, static_cast<u32>(sizeof(Mp3DecoderState)));
    *decoder = {storage.data, static_cast<u32>(sizeof(Mp3DecoderState)), rate, static_cast<u8>(channels),
                true};
    return Mp3Error::none;
}
Mp3Error mp3_decoder_reset(Mp3Decoder* decoder) noexcept {
    if (!decoder_current(decoder))
        return Mp3Error::invalid_input;
    clear_bytes(decoder->state, decoder->state_bytes);
    return Mp3Error::none;
}
Mp3DecodeResult mp3_decoder_decode(Mp3Decoder* decoder, ConstSlice<u8> packet, Slice<float> pcm) noexcept {
    const usize pcm_bytes = static_cast<usize>(pcm.count) * sizeof(float);
    if (!decoder_current(decoder) || !packet.data || !packet.count || !pcm.data ||
        (reinterpret_cast<usize>(pcm.data) & (alignof(float) - 1)) ||
        byte_ranges_overlap(decoder->state, decoder->state_bytes, packet.data, packet.count) ||
        byte_ranges_overlap(decoder->state, decoder->state_bytes, pcm.data, pcm_bytes) ||
        byte_ranges_overlap(packet.data, packet.count, pcm.data, pcm_bytes) ||
        byte_ranges_overlap(decoder, sizeof(*decoder), packet.data, packet.count) ||
        byte_ranges_overlap(decoder, sizeof(*decoder), pcm.data, pcm_bytes))
        return {Mp3Error::invalid_input};
    Mp3Frame f{};
    const Mp3Error error = frame(packet, 0, &f);
    if (error != Mp3Error::none)
        return {error};
    if (f.bytes != packet.count)
        return {Mp3Error::malformed};
    if (f.sample_rate != decoder->sample_rate || f.channels != decoder->channels)
        return {Mp3Error::unsupported};
    if (pcm.count / decoder->channels < f.samples)
        return {Mp3Error::capacity};
    // Every sample is finite: dequantized lines stay below 2^30 and each stage
    // sums a bounded number of them with bounded weights.
    const u32 frames =
        mp3_frame_decode(static_cast<Mp3DecoderState*>(decoder->state), packet.data, &f, pcm.data);
    return frames ? Mp3DecodeResult{Mp3Error::none, frames} : Mp3DecodeResult{Mp3Error::need_preroll};
}
void mp3_decoder_release(Mp3Decoder* decoder) noexcept {
    if (decoder)
        reset_value(decoder);
}
} // namespace maicord
