#include "maicord/media/mp4_demux.hpp"
#include "maicord/media/mp4_audio_demux.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/media/encoded_audio.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/media/video_display.hpp"
#include "../src/media/mp4_demux_detail.hpp"

namespace maicord {
namespace {

struct Writer {
    u8 bytes[4'096];
    u32 count;
    bool ok;
    u32 edit_span_offset, edit_media_offset, composition_count_offset, alias_flags_offset;
};
// QuickTime movie shape for the shared fixtures: 'qt  ' brand, an 'alis'
// self-reference, SoundDescription v1/v2 and a 'wave'-wrapped esds.
bool g_quicktime;
u16 g_sound_version;
bool g_sound_wave;

void put_u8(Writer* writer, u8 value) noexcept {
    if (!writer->ok || writer->count >= sizeof(writer->bytes)) {
        writer->ok = false;
        return;
    }
    writer->bytes[writer->count++] = value;
}

void put_u16(Writer* writer, u16 value) noexcept {
    put_u8(writer, static_cast<u8>(value >> 8u));
    put_u8(writer, static_cast<u8>(value));
}

void put_u32(Writer* writer, u32 value) noexcept {
    put_u8(writer, static_cast<u8>(value >> 24u));
    put_u8(writer, static_cast<u8>(value >> 16u));
    put_u8(writer, static_cast<u8>(value >> 8u));
    put_u8(writer, static_cast<u8>(value));
}

void patch_u32(Writer* writer, u32 offset, u32 value) noexcept {
    if (offset > writer->count || writer->count - offset < 4u) {
        writer->ok = false;
        return;
    }
    writer->bytes[offset + 0u] = static_cast<u8>(value >> 24u);
    writer->bytes[offset + 1u] = static_cast<u8>(value >> 16u);
    writer->bytes[offset + 2u] = static_cast<u8>(value >> 8u);
    writer->bytes[offset + 3u] = static_cast<u8>(value);
}

u32 begin_box(Writer* writer, char a, char b, char c, char d) noexcept {
    const u32 begin = writer->count;
    put_u32(writer, 0);
    put_u8(writer, static_cast<u8>(a));
    put_u8(writer, static_cast<u8>(b));
    put_u8(writer, static_cast<u8>(c));
    put_u8(writer, static_cast<u8>(d));
    return begin;
}

void end_box(Writer* writer, u32 begin) noexcept {
    patch_u32(writer, begin, writer->count - begin);
}

void put_zeroes(Writer* writer, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        put_u8(writer, 0);
    }
}

bool fixture(Writer* writer, u32 sar_width = 0, u32 sar_height = 0, u32 pasp_count = 0, u32 pasp_bytes = 8,
             bool composition_present = false, i32 composition = 0, i32 trim = -1, u32 samples = 1,
             u32 second_idr = ~u32{0}, u32 empty_ticks = 0, u32 edit_span = 0,
             const i32* composition_offsets = nullptr, bool unsigned_composition = false,
             bool hevc = false) noexcept {
    if (!samples || samples > 5 || empty_ticks > 1000 || (empty_ticks && trim < 0) ||
        (trim >= 0 && !edit_span && static_cast<u32>(trim) >= samples * 100) ||
        (second_idr != ~u32{0} && (!second_idr || second_idr >= samples)))
        return false;
    const u32 duration = samples * 100;
    *writer = Writer{};
    writer->ok = true;
    const u32 ftyp = begin_box(writer, 'f', 't', 'y', 'p');
    put_u32(writer, g_quicktime ? 0x71742020u : 0x69736f6du);
    put_u32(writer, 0x200u);
    put_u32(writer, g_quicktime ? 0x71742020u : 0x61766331u);
    end_box(writer, ftyp);

    const u32 moov = begin_box(writer, 'm', 'o', 'o', 'v');
    if (trim >= 0) {
        const u32 mvhd = begin_box(writer, 'm', 'v', 'h', 'd');
        put_zeroes(writer, 12);
        put_u32(writer, 1000);
        put_u32(writer, duration + empty_ticks);
        end_box(writer, mvhd);
    }
    const u32 trak = begin_box(writer, 't', 'r', 'a', 'k');
    if (trim >= 0) {
        const u32 edts = begin_box(writer, 'e', 'd', 't', 's'),
                  elst = begin_box(writer, 'e', 'l', 's', 't');
        put_u32(writer, 0);
        put_u32(writer, empty_ticks ? 2u : 1u);
        if (empty_ticks) {
            put_u32(writer, empty_ticks);
            put_u32(writer, ~u32{0});
            put_u32(writer, 0x00010000);
        }
        writer->edit_span_offset = writer->count;
        put_u32(writer, edit_span ? edit_span : duration - static_cast<u32>(trim));
        writer->edit_media_offset = writer->count;
        put_u32(writer, static_cast<u32>(trim));
        put_u32(writer, 0x00010000);
        end_box(writer, elst);
        end_box(writer, edts);
    }
    const u32 mdia = begin_box(writer, 'm', 'd', 'i', 'a');
    const u32 mdhd = begin_box(writer, 'm', 'd', 'h', 'd');
    put_u32(writer, 0);
    put_u32(writer, 0);
    put_u32(writer, 0);
    put_u32(writer, 1'000);
    put_u32(writer, duration);
    put_u16(writer, 0);
    put_u16(writer, 0);
    end_box(writer, mdhd);
    const u32 hdlr = begin_box(writer, 'h', 'd', 'l', 'r');
    put_u32(writer, 0);
    put_u32(writer, 0);
    put_u32(writer, 0x76696465u);
    end_box(writer, hdlr);
    const u32 minf = begin_box(writer, 'm', 'i', 'n', 'f');
    if (g_quicktime) {
        // QuickTime's data handler and self-contained alias reference.
        const u32 dhlr = begin_box(writer, 'h', 'd', 'l', 'r');
        put_u32(writer, 0);
        put_u32(writer, 0x64686c72u);
        put_u32(writer, 0x616c6973u);
        put_zeroes(writer, 13);
        end_box(writer, dhlr);
        const u32 dinf = begin_box(writer, 'd', 'i', 'n', 'f'), dref = begin_box(writer, 'd', 'r', 'e', 'f');
        put_u32(writer, 0);
        put_u32(writer, 1);
        const u32 alis = begin_box(writer, 'a', 'l', 'i', 's');
        writer->alias_flags_offset = writer->count;
        put_u32(writer, 1);
        end_box(writer, alis);
        end_box(writer, dref);
        end_box(writer, dinf);
    }
    const u32 stbl = begin_box(writer, 's', 't', 'b', 'l');
    const u32 stsd = begin_box(writer, 's', 't', 's', 'd');
    put_u32(writer, 0);
    put_u32(writer, 1);
    const u32 avc1 = begin_box(writer, hevc ? 'h' : 'a', 'v', 'c', '1');
    put_zeroes(writer, 78);
    writer->bytes[avc1 + 8u + 24u] = 0;
    writer->bytes[avc1 + 8u + 25u] = 16;
    writer->bytes[avc1 + 8u + 26u] = 0;
    writer->bytes[avc1 + 8u + 27u] = 16;
    const u32 avcc = begin_box(writer, hevc ? 'h' : 'a', 'v', 'c', 'C');
    if (hevc) {
        // Container metadata only. The timestamp test does not decode NALs.
        constexpr u8 configuration[]{1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30,
                                    0xf0, 0, 0xfc, 0xfd, 0xf8, 0xf8, 0, 0, 15, 0};
        for (u8 value : configuration)
            put_u8(writer, value);
    } else {
        put_u8(writer, 1);
        put_u8(writer, 66);
        put_u8(writer, 0);
        put_u8(writer, 30);
        put_u8(writer, 0xff);
        put_u8(writer, 0xe1);
        put_u16(writer, 2);
        put_u8(writer, 0x67);
        put_u8(writer, 0);
        put_u8(writer, 1);
        put_u16(writer, 2);
        put_u8(writer, 0x68);
        put_u8(writer, 0);
    }
    end_box(writer, avcc);
    for (u32 i = 0; i < pasp_count; ++i) {
        const u32 pasp = begin_box(writer, 'p', 'a', 's', 'p');
        if (pasp_bytes >= 4)
            put_u32(writer, sar_width);
        if (pasp_bytes >= 8)
            put_u32(writer, sar_height);
        if (pasp_bytes > 8)
            put_zeroes(writer, pasp_bytes - 8);
        end_box(writer, pasp);
    }
    end_box(writer, avc1);
    end_box(writer, stsd);

    const u32 stts = begin_box(writer, 's', 't', 't', 's');
    put_u32(writer, 0);
    put_u32(writer, 1);
    put_u32(writer, samples);
    put_u32(writer, 100);
    end_box(writer, stts);
    if (composition_present) {
        const u32 ctts = begin_box(writer, 'c', 't', 't', 's');
        put_u32(writer, unsigned_composition ? 0u : 0x01000000u);
        put_u32(writer, composition_offsets ? samples : 1u);
        writer->composition_count_offset = writer->count;
        if (composition_offsets) {
            for (u32 i = 0; i < samples; ++i) {
                put_u32(writer, 1);
                put_u32(writer, static_cast<u32>(composition_offsets[i]));
            }
        } else {
            put_u32(writer, samples);
            put_u32(writer, static_cast<u32>(composition));
        }
        end_box(writer, ctts);
    }
    const u32 stsc = begin_box(writer, 's', 't', 's', 'c');
    put_u32(writer, 0);
    put_u32(writer, 1);
    put_u32(writer, 1);
    put_u32(writer, samples);
    put_u32(writer, 1);
    end_box(writer, stsc);
    const u32 stsz = begin_box(writer, 's', 't', 's', 'z');
    put_u32(writer, 0);
    put_u32(writer, 0);
    put_u32(writer, samples);
    for (u32 i = 0; i < samples; ++i)
        put_u32(writer, 6);
    end_box(writer, stsz);
    const u32 stco = begin_box(writer, 's', 't', 'c', 'o');
    put_u32(writer, 0);
    put_u32(writer, 1);
    const u32 chunk_patch = writer->count;
    put_u32(writer, 0);
    end_box(writer, stco);
    const u32 stss = begin_box(writer, 's', 't', 's', 's');
    put_u32(writer, 0);
    put_u32(writer, second_idr == ~u32{0} ? 1u : 2u);
    put_u32(writer, 1);
    if (second_idr != ~u32{0})
        put_u32(writer, second_idr + 1);
    end_box(writer, stss);
    end_box(writer, stbl);
    end_box(writer, minf);
    end_box(writer, mdia);
    end_box(writer, trak);
    end_box(writer, moov);

    const u32 mdat = begin_box(writer, 'm', 'd', 'a', 't');
    const u32 sample_offset = writer->count;
    for (u32 i = 0; i < samples; ++i) {
        put_u32(writer, 2);
        put_u8(writer, (i == 0 || i == second_idr) ? u8{0x65} : u8{0x41});
        put_u8(writer, 0x88);
    }
    end_box(writer, mdat);
    patch_u32(writer, chunk_patch, sample_offset);
    return writer->ok;
}

int composition_edit_tests() noexcept {
    Writer writer{};
    Mp4Demuxer demux{};
    // Original container metadata: decoding spans 0..500 ticks, while
    // composition spans 100..600. The edit removes the CTS offset without
    // shortening the five displayed samples. No encoded media is copied.
    for (u32 version = 0; version < 2; ++version) {
        if (!fixture(&writer, 0, 0, 0, 8, true, 100, 100, 5, ~u32{0}, 0, 500, nullptr, version == 0) ||
            mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
            demux.media_duration_ticks != 500 || demux.media_start_ticks != 100 ||
            demux.movie_duration_ticks != 500 || demux.duration_us != 500'000 || !demux.has_edit_list)
            return 60;
        for (u32 sample = 0; sample < 5; ++sample) {
            const auto unit = mp4_demux_next(&demux);
            if (!unit.ok() || unit.sample_index != sample || unit.decode_time_ticks != sample * 100 ||
                unit.presentation_time_ticks != 100 + sample * 100 ||
                unit.presentation_time_us_signed != static_cast<i64>(sample) * 100'000 ||
                unit.duration_ticks != 100)
                return 61;
        }
        if (mp4_demux_next(&demux).error != Mp4DemuxError::end_of_stream)
            return 62;
        Mp4SeekPoint point{};
        if (mp4_demux_seek_sync(&demux, 500'000, &point) != Mp4DemuxError::none ||
            point.sample_index != 0 || point.presentation_time_us != 0)
            return 63;
        // Preserve the existing one-movie-tick rounding allowance, then
        // reject an edit extending beyond the actual composition endpoint.
        patch_u32(&writer, writer.edit_span_offset, 501);
        if (mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none)
            return 64;
        patch_u32(&writer, writer.edit_span_offset, 502);
        if (mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::unsupported ||
            demux.ready)
            return 65;
        patch_u32(&writer, writer.edit_span_offset, ~u32{0});
        if (mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::unsupported ||
            demux.ready)
            return 66;
    }
    // Decode order differs from presentation order, as in a B-frame track.
    // The final presentation endpoint is not necessarily the final sample.
    const i32 offsets[]{200, 500, 100, 100, 100};
    const i64 expected[]{0, 400'000, 100'000, 200'000, 300'000};
    if (!fixture(&writer, 0, 0, 0, 8, true, 0, 200, 5, ~u32{0}, 0, 500, offsets, true) ||
        mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
        demux.duration_us != 500'000 || demux.media_duration_ticks != 500)
        return 67;
    for (u32 sample = 0; sample < 5; ++sample) {
        const auto unit = mp4_demux_next(&demux);
        if (!unit.ok() || unit.presentation_time_us_signed != expected[sample])
            return 68;
    }
    patch_u32(&writer, writer.composition_count_offset, 2);
    if (mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::malformed ||
        demux.ready)
        return 69;
    if (!fixture(&writer, 0, 0, 0, 8, true, 100, 600, 5, ~u32{0}, 0, 1) ||
        mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::unsupported ||
        demux.ready)
        return 70;
    if (!fixture(&writer, 0, 0, 0, 8, true, -100, 500, 5, ~u32{0}, 0, 1) ||
        mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::unsupported ||
        demux.ready)
        return 71;
    // Leading empty edit is independent from CTS delay removal.
    if (!fixture(&writer, 0, 0, 0, 8, true, 100, 100, 5, ~u32{0}, 80, 500) ||
        mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
        demux.duration_us != 580'000 || mp4_demux_next(&demux).presentation_time_us_signed != 80'000)
        return 72;
    // Retain the existing negative-CTS trailing-duration admission, including
    // the original GPU-State leading-gap fixture's independent empty edit.
    if (!fixture(&writer, 0, 0, 0, 8, true, -20, 0, 5, ~u32{0}, 100, 500) ||
        mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
        demux.duration_us != 600'000 || mp4_demux_next(&demux).presentation_time_us_signed != 80'000)
        return 73;
    return 0;
}

bool audio_fixture(Writer& w, u32 rate = 48000, u8 channels = 2, u8 asc_flags = 0, bool trim = false,
                   u32 movie_scale = 0, u32 empty_ticks = 0, u32 span_ticks = 0, u32 start_ticks = 1024,
                   u8 composition_kind = 0, ConstSlice<u8> configuration = {}) noexcept {
    if (!movie_scale)
        movie_scale = rate;
    if (!span_ticks)
        span_ticks = 2048;
    w = {};
    w.ok = true;
    const u32 moov = begin_box(&w, 'm', 'o', 'o', 'v');
    if (trim) {
        const u32 mvhd = begin_box(&w, 'm', 'v', 'h', 'd');
        put_zeroes(&w, 12);
        put_u32(&w, movie_scale);
        put_u32(&w, span_ticks + empty_ticks);
        end_box(&w, mvhd);
    }
    const u32 trak = begin_box(&w, 't', 'r', 'a', 'k');
    if (trim) {
        const u32 edts = begin_box(&w, 'e', 'd', 't', 's'), elst = begin_box(&w, 'e', 'l', 's', 't');
        put_u32(&w, 0);
        put_u32(&w, empty_ticks ? 2u : 1u);
        if (empty_ticks) {
            put_u32(&w, empty_ticks);
            put_u32(&w, ~u32{0});
            put_u32(&w, 0x00010000);
        }
        put_u32(&w, span_ticks);
        put_u32(&w, start_ticks);
        put_u32(&w, 0x00010000);
        end_box(&w, elst);
        end_box(&w, edts);
    }
    const u32 mdia = begin_box(&w, 'm', 'd', 'i', 'a'), mdhd = begin_box(&w, 'm', 'd', 'h', 'd');
    put_zeroes(&w, 12);
    put_u32(&w, rate);
    put_u32(&w, 3072);
    put_zeroes(&w, 4);
    end_box(&w, mdhd);
    const u32 hdlr = begin_box(&w, 'h', 'd', 'l', 'r');
    put_zeroes(&w, 8);
    put_u32(&w, 0x736f756e);
    end_box(&w, hdlr);
    const u32 minf = begin_box(&w, 'm', 'i', 'n', 'f'), stbl = begin_box(&w, 's', 't', 'b', 'l'),
              stsd = begin_box(&w, 's', 't', 's', 'd');
    put_u32(&w, 0);
    put_u32(&w, 1);
    const u32 mp4a = begin_box(&w, 'm', 'p', '4', 'a');
    put_zeroes(&w, 6);
    put_u16(&w, 1);
    put_u16(&w, g_sound_version);
    put_zeroes(&w, 6);
    if (g_sound_version == 2) {
        // Version 2 moves rate and channels into its extension; fixed legacy values.
        put_u16(&w, 3);
        put_u16(&w, 16);
        put_u16(&w, 0xfffe);
        put_u16(&w, 0);
        put_u32(&w, 0x00010000);
        put_u32(&w, 72);
        double sample_rate = rate;
        u64 bits{};
        copy_bytes(&bits, &sample_rate, sizeof(bits));
        put_u32(&w, static_cast<u32>(bits >> 32));
        put_u32(&w, static_cast<u32>(bits));
        put_u32(&w, channels);
        put_u32(&w, 0x7f000000);
        put_zeroes(&w, 12);
        put_u32(&w, 1024);
    } else {
        put_u16(&w, channels);
        put_u16(&w, 16);
        put_zeroes(&w, 4);
        put_u32(&w, rate << 16);
        if (g_sound_version == 1) {
            put_u32(&w, 1024);
            put_zeroes(&w, 4);
            put_u32(&w, 2u * channels);
            put_u32(&w, 2);
        }
    }
    const u32 wave = g_sound_wave ? begin_box(&w, 'w', 'a', 'v', 'e') : 0;
    if (g_sound_wave) {
        const u32 frma = begin_box(&w, 'f', 'r', 'm', 'a');
        put_u32(&w, 0x6d703461u);
        end_box(&w, frma);
    }
    const u32 esds = begin_box(&w, 'e', 's', 'd', 's');
    put_u32(&w, 0);
    const u32 asc_bytes = configuration.count ? configuration.count : 2;
    put_u8(&w, 3);
    put_u8(&w, static_cast<u8>(23 + asc_bytes));
    put_u16(&w, 1);
    put_u8(&w, 0);
    put_u8(&w, 4);
    put_u8(&w, static_cast<u8>(15 + asc_bytes));
    put_u8(&w, 0x40);
    put_u8(&w, 0x15);
    put_zeroes(&w, 3);
    put_u32(&w, 64000);
    put_u32(&w, 64000);
    put_u8(&w, 5);
    put_u8(&w, static_cast<u8>(asc_bytes));
    const u16 asc =
        static_cast<u16>((2u << 11) | ((rate == 44100 ? 4u : 3u) << 7) | (u32{channels} << 3) | asc_flags);
    if (configuration.count) {
        for (u32 i = 0; i < configuration.count; ++i)
            put_u8(&w, configuration[i]);
    } else
        put_u16(&w, asc);
    put_u8(&w, 6);
    put_u8(&w, 1);
    put_u8(&w, 2);
    end_box(&w, esds);
    if (g_sound_wave) {
        put_u32(&w, 8);
        put_u32(&w, 0);
        end_box(&w, wave);
    }
    end_box(&w, mp4a);
    end_box(&w, stsd);
    const u32 stts = begin_box(&w, 's', 't', 't', 's');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1024);
    end_box(&w, stts);
    if (composition_kind) {
        const bool split = composition_kind >= 4;
        const u32 ctts = begin_box(&w, 'c', 't', 't', 's');
        put_u32(&w, (composition_kind == 3 || composition_kind == 5) ? 0x01000000u : 0u);
        put_u32(&w, split ? 2u : 1u);
        put_u32(&w, split ? 2u : 3u);
        put_u32(&w, composition_kind == 2 ? 1u : composition_kind == 3 ? ~u32{0} : 0u);
        if (split) {
            put_u32(&w, 1);
            put_u32(&w, composition_kind == 4 ? 1u : 0u);
        }
        end_box(&w, ctts);
    }
    const u32 stsc = begin_box(&w, 's', 't', 's', 'c');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1);
    end_box(&w, stsc);
    const u32 stsz = begin_box(&w, 's', 't', 's', 'z');
    put_u32(&w, 0);
    put_u32(&w, 4);
    put_u32(&w, 3);
    end_box(&w, stsz);
    const u32 stco = begin_box(&w, 's', 't', 'c', 'o');
    put_u32(&w, 0);
    put_u32(&w, 1);
    const u32 patch = w.count;
    put_u32(&w, 0);
    end_box(&w, stco);
    end_box(&w, stbl);
    end_box(&w, minf);
    end_box(&w, mdia);
    end_box(&w, trak);
    end_box(&w, moov);
    const u32 mdat = begin_box(&w, 'm', 'd', 'a', 't');
    patch_u32(&w, patch, w.count);
    for (u32 i = 0; i < 12; ++i)
        put_u8(&w, static_cast<u8>(i + 1));
    end_box(&w, mdat);
    return w.ok;
}

int quicktime_tests() noexcept {
    Writer writer{};
    Mp4Demuxer demux{};
    g_quicktime = true;
    const bool built = fixture(&writer, 0, 0, 0, 8, false, 0, -1, 3);
    g_quicktime = false;
    if (!built || mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::none)
        return 80;
    for (u32 sample = 0; sample < 3; ++sample)
        if (!mp4_demux_next(&demux).ok())
            return 81;
    // An alias without the self-contained flag names another file.
    patch_u32(&writer, writer.alias_flags_offset, 0);
    if (mp4_demux_initialize(&demux, {writer.bytes, writer.count}) != Mp4DemuxError::unsupported || demux.ready)
        return 82;
    Writer w{};
    Mp4AudioDemuxer audio{};
    for (u16 version = 0; version < 3; ++version) {
        for (u32 wave = 0; wave < 2; ++wave) {
            g_sound_version = version;
            g_sound_wave = wave != 0;
            const bool audio_built = audio_fixture(w, 44100, 1);
            g_sound_version = 0;
            g_sound_wave = false;
            if (!audio_built || mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::none ||
                audio.sample_rate != 44100 || audio.channels != 1)
                return 83;
        }
    }
    // The same demuxer is the m4a attachment metadata the playback session uses.
    if (!audio_fixture(w) || mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 85;
    const EncodedAudioMetadata m4a{audio};
    if (!encoded_audio_ready(m4a) || encoded_audio_rate(m4a) != 48000 || encoded_audio_channels(m4a) != 2 ||
        encoded_audio_total_frames(m4a) != 3072 || encoded_audio_bytes(m4a).data != w.bytes ||
        encoded_audio_duration_ms(m4a) != 64)
        return 86;
    g_sound_version = 3;
    const bool future = audio_fixture(w);
    g_sound_version = 0;
    if (!future || mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::unsupported)
        return 84;
    return 0;
}

// The track layout of an iPhone QuickTime movie: 'qt  ' brand, timecode and
// metadata tracks, a disabled alternate per handler ahead of the decodable
// one, tkhd rotation, tapt, 'wide', a 64-bit mdat header, colr and a bare
// zero terminator closing the visual entry, ctts v1, sdtp, sgpd/sbgp,
// co64, and SoundDescription v1 with a 'wave'-wrapped esds.
struct MovieSpec {
    i32 matrix[4]{0x10000, 0, 0, 0x10000};
    u32 terminator{};
    bool terminated{true};
    bool co64{};
    bool hevc{};
    bool alternates{true};
    bool aac{true};
};
// Offset of the fixture's DecoderConfigDescriptor stream type byte.
u32 g_movie_stream_type;
void put_handler(Writer* w, u32 component, u32 subtype) noexcept {
    const u32 hdlr = begin_box(w, 'h', 'd', 'l', 'r');
    put_u32(w, 0);
    put_u32(w, component);
    put_u32(w, subtype);
    put_zeroes(w, 13);
    end_box(w, hdlr);
}
void put_track_header(Writer* w, u32 flags, const i32* matrix, u32 width, u32 height) noexcept {
    const u32 tkhd = begin_box(w, 't', 'k', 'h', 'd');
    put_u32(w, flags);
    put_zeroes(w, 8);
    put_u32(w, 1);
    put_zeroes(w, 4);
    put_u32(w, 300);
    put_zeroes(w, 16);
    const i32 full[9]{matrix[0], matrix[1], 0, matrix[2], matrix[3], 0, 0, 0, 0x40000000};
    for (i32 value : full)
        put_u32(w, static_cast<u32>(value));
    put_u32(w, width << 16);
    put_u32(w, height << 16);
    end_box(w, tkhd);
}
void put_media_header(Writer* w, u32 timescale, u32 duration) noexcept {
    const u32 mdhd = begin_box(w, 'm', 'd', 'h', 'd');
    put_zeroes(w, 12);
    put_u32(w, timescale);
    put_u32(w, duration);
    put_zeroes(w, 4);
    end_box(w, mdhd);
}
void put_alias_reference(Writer* w) noexcept {
    put_handler(w, 0x64686c72u, 0x616c6973u);
    const u32 dinf = begin_box(w, 'd', 'i', 'n', 'f'), dref = begin_box(w, 'd', 'r', 'e', 'f');
    put_u32(w, 0);
    put_u32(w, 1);
    const u32 alis = begin_box(w, 'a', 'l', 'i', 's');
    put_u32(w, 1);
    end_box(w, alis);
    end_box(w, dref);
    end_box(w, dinf);
}
void put_edit(Writer* w, u32 span, u32 media) noexcept {
    const u32 edts = begin_box(w, 'e', 'd', 't', 's'), elst = begin_box(w, 'e', 'l', 's', 't');
    put_u32(w, 0);
    put_u32(w, 1);
    put_u32(w, span);
    put_u32(w, media);
    put_u32(w, 0x00010000);
    end_box(w, elst);
    end_box(w, edts);
}
// Returns the offset of the single chunk entry to patch.
u32 put_chunk_table(Writer* w, bool co64) noexcept {
    const u32 table = co64 ? begin_box(w, 'c', 'o', '6', '4') : begin_box(w, 's', 't', 'c', 'o');
    put_u32(w, 0);
    put_u32(w, 1);
    const u32 patch = w->count;
    put_u32(w, 0);
    if (co64)
        put_u32(w, 0);
    end_box(w, table);
    return patch;
}
// A track the selection must skip: only its handler is ever read.
void put_handler_track(Writer* w, u32 subtype) noexcept {
    const u32 trak = begin_box(w, 't', 'r', 'a', 'k'), mdia = begin_box(w, 'm', 'd', 'i', 'a');
    put_handler(w, 0x6d686c72u, subtype);
    end_box(w, mdia);
    end_box(w, trak);
}
// A disabled alternate whose codec has no decoder here.
void put_alternate_track(Writer* w, u32 handler, char a, char b, char c, char d, u32 entry_bytes) noexcept {
    constexpr i32 identity[4]{0x10000, 0, 0, 0x10000};
    const u32 trak = begin_box(w, 't', 'r', 'a', 'k');
    put_track_header(w, 0, identity, 0, 0);
    const u32 mdia = begin_box(w, 'm', 'd', 'i', 'a');
    put_media_header(w, 1000, 300);
    put_handler(w, 0x6d686c72u, handler);
    const u32 minf = begin_box(w, 'm', 'i', 'n', 'f'), stbl = begin_box(w, 's', 't', 'b', 'l'),
              stsd = begin_box(w, 's', 't', 's', 'd');
    put_u32(w, 0);
    put_u32(w, 1);
    const u32 entry = begin_box(w, a, b, c, d);
    put_zeroes(w, entry_bytes);
    end_box(w, entry);
    end_box(w, stsd);
    end_box(w, stbl);
    end_box(w, minf);
    end_box(w, mdia);
    end_box(w, trak);
}
bool movie_fixture(Writer& w, const MovieSpec& spec) noexcept {
    w = {};
    w.ok = true;
    const u32 ftyp = begin_box(&w, 'f', 't', 'y', 'p');
    put_u32(&w, 0x71742020u);
    put_u32(&w, 0);
    put_u32(&w, 0x71742020u);
    end_box(&w, ftyp);
    const u32 moov = begin_box(&w, 'm', 'o', 'o', 'v');
    const u32 mvhd = begin_box(&w, 'm', 'v', 'h', 'd');
    put_zeroes(&w, 12);
    put_u32(&w, 1000);
    put_u32(&w, 300);
    put_zeroes(&w, 80);
    end_box(&w, mvhd);
    put_handler_track(&w, 0x746d6364u);  // 'tmcd' timecode
    if (spec.alternates) {
        put_alternate_track(&w, 0x76696465u, 'm', 'p', '4', 'v', 78);
        put_alternate_track(&w, 0x736f756eu, 'a', 'l', 'a', 'c', 28);
    }
    // Video: 32x16, three 100 ms samples decoded 0/100/200 and shown
    // 0/200/100 through signed (version 1) composition offsets.
    const u32 video = begin_box(&w, 't', 'r', 'a', 'k');
    put_track_header(&w, 15, spec.matrix, 32, 16);
    const u32 tapt = begin_box(&w, 't', 'a', 'p', 't'), clef = begin_box(&w, 'c', 'l', 'e', 'f');
    put_u32(&w, 0);
    put_u32(&w, 32u << 16);
    put_u32(&w, 16u << 16);
    end_box(&w, clef);
    end_box(&w, tapt);
    put_edit(&w, 300, 0);
    const u32 video_media = begin_box(&w, 'm', 'd', 'i', 'a');
    put_media_header(&w, 1000, 300);
    put_handler(&w, 0x6d686c72u, 0x76696465u);
    const u32 video_info = begin_box(&w, 'm', 'i', 'n', 'f');
    const u32 vmhd = begin_box(&w, 'v', 'm', 'h', 'd');
    put_u32(&w, 1);
    put_zeroes(&w, 8);
    end_box(&w, vmhd);
    put_alias_reference(&w);
    const u32 video_table = begin_box(&w, 's', 't', 'b', 'l'), video_stsd = begin_box(&w, 's', 't', 's', 'd');
    put_u32(&w, 0);
    put_u32(&w, 1);
    const u32 visual = begin_box(&w, spec.hevc ? 'h' : 'a', 'v', 'c', '1');
    put_zeroes(&w, 78);
    w.bytes[visual + 8u + 25u] = 32;
    w.bytes[visual + 8u + 27u] = 16;
    const u32 configuration = begin_box(&w, spec.hevc ? 'h' : 'a', 'v', 'c', 'C');
    if (spec.hevc) {
        constexpr u8 hvcc[]{1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 30, 0xf0, 0, 0xfc, 0xfd, 0xf8, 0xf8, 0, 0, 15, 0};
        for (u8 value : hvcc)
            put_u8(&w, value);
    } else {
        constexpr u8 avcc[]{1, 66, 0, 30, 0xff, 0xe1, 0, 2, 0x67, 0, 1, 0, 2, 0x68, 0};
        for (u8 value : avcc)
            put_u8(&w, value);
    }
    end_box(&w, configuration);
    const u32 colr = begin_box(&w, 'c', 'o', 'l', 'r');
    put_u32(&w, 0x6e636c63u);  // QuickTime 'nclc': BT.709 primaries/transfer/matrix
    put_u16(&w, 1);
    put_u16(&w, 1);
    put_u16(&w, 1);
    end_box(&w, colr);
    if (spec.terminated)
        put_u32(&w, spec.terminator);
    end_box(&w, visual);
    end_box(&w, video_stsd);
    const u32 stts = begin_box(&w, 's', 't', 't', 's');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 100);
    end_box(&w, stts);
    const u32 ctts = begin_box(&w, 'c', 't', 't', 's');
    put_u32(&w, 0x01000000u);
    put_u32(&w, 3);
    constexpr i32 offsets[]{0, 100, -100};
    for (i32 offset : offsets) {
        put_u32(&w, 1);
        put_u32(&w, static_cast<u32>(offset));
    }
    end_box(&w, ctts);
    const u32 stss = begin_box(&w, 's', 't', 's', 's');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 1);
    end_box(&w, stss);
    const u32 sdtp = begin_box(&w, 's', 'd', 't', 'p');
    put_u32(&w, 0);
    put_u8(&w, 0x20);
    put_u8(&w, 0x10);
    put_u8(&w, 0x18);
    end_box(&w, sdtp);
    const u32 video_stsc = begin_box(&w, 's', 't', 's', 'c');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1);
    end_box(&w, video_stsc);
    const u32 video_stsz = begin_box(&w, 's', 't', 's', 'z');
    put_u32(&w, 0);
    put_u32(&w, 6);
    put_u32(&w, 3);
    end_box(&w, video_stsz);
    const u32 video_patch = put_chunk_table(&w, spec.co64);
    end_box(&w, video_table);
    end_box(&w, video_info);
    end_box(&w, video_media);
    end_box(&w, video);
    put_handler_track(&w, 0x6d657461u);  // 'meta' (mebx) metadata
    // Audio: AAC-LC 44.1 kHz stereo, three packets, 1024 priming frames.
    const u32 sound = begin_box(&w, 't', 'r', 'a', 'k');
    constexpr i32 identity[4]{0x10000, 0, 0, 0x10000};
    put_track_header(&w, 15, identity, 0, 0);
    put_edit(&w, 46, 1024);
    const u32 sound_media = begin_box(&w, 'm', 'd', 'i', 'a');
    put_media_header(&w, 44100, 3072);
    put_handler(&w, 0x6d686c72u, 0x736f756eu);
    const u32 sound_info = begin_box(&w, 'm', 'i', 'n', 'f'), smhd = begin_box(&w, 's', 'm', 'h', 'd');
    put_zeroes(&w, 8);
    end_box(&w, smhd);
    put_alias_reference(&w);
    const u32 sound_table = begin_box(&w, 's', 't', 'b', 'l'), sound_stsd = begin_box(&w, 's', 't', 's', 'd');
    put_u32(&w, 0);
    put_u32(&w, 1);
    const u32 mp4a = begin_box(&w, 'm', 'p', '4', 'a');
    put_zeroes(&w, 6);
    put_u16(&w, 1);
    put_u16(&w, 1);
    put_zeroes(&w, 6);
    put_u16(&w, 2);
    put_u16(&w, 16);
    put_u16(&w, 0xfffe);
    put_u16(&w, 0);
    put_u32(&w, 44100u << 16);
    put_u32(&w, 1024);
    put_u32(&w, 0);
    put_u32(&w, 4);
    put_u32(&w, 2);
    const u32 wave = begin_box(&w, 'w', 'a', 'v', 'e'), frma = begin_box(&w, 'f', 'r', 'm', 'a');
    put_u32(&w, 0x6d703461u);
    end_box(&w, frma);
    const u32 inner = begin_box(&w, 'm', 'p', '4', 'a');
    put_u32(&w, 0);
    end_box(&w, inner);
    const u32 esds = begin_box(&w, 'e', 's', 'd', 's');
    // Apple's DecoderConfigDescriptor: stream type byte 0x14 (reserved bit clear).
    constexpr u8 descriptors[]{0, 0, 0, 0, 3, 25, 0, 1, 0, 4, 17, 0x40, 0x14, 0, 0, 0, 0, 0, 0xfa, 0,
                               0, 0, 0xfa, 0, 5, 2, 0x12, 0x10, 6, 1, 2};
    g_movie_stream_type = w.count + 12;
    for (u8 value : descriptors)
        put_u8(&w, value);
    end_box(&w, esds);
    put_u32(&w, 8);  // null-atom terminator of 'wave'
    put_u32(&w, 0);
    end_box(&w, wave);
    put_u32(&w, 0);  // and a bare terminator closing the sound entry
    end_box(&w, mp4a);
    end_box(&w, sound_stsd);
    const u32 sgpd = begin_box(&w, 's', 'g', 'p', 'd');
    put_u32(&w, 0x01000000u);
    put_u32(&w, 0x726f6c6cu);
    put_u32(&w, 2);
    put_u32(&w, 1);
    put_u16(&w, 0xffff);
    end_box(&w, sgpd);
    const u32 sbgp = begin_box(&w, 's', 'b', 'g', 'p');
    put_u32(&w, 0);
    put_u32(&w, 0x726f6c6cu);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1);
    end_box(&w, sbgp);
    const u32 sound_stts = begin_box(&w, 's', 't', 't', 's');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1024);
    end_box(&w, sound_stts);
    const u32 sound_stsc = begin_box(&w, 's', 't', 's', 'c');
    put_u32(&w, 0);
    put_u32(&w, 1);
    put_u32(&w, 1);
    put_u32(&w, 3);
    put_u32(&w, 1);
    end_box(&w, sound_stsc);
    const u32 sound_stsz = begin_box(&w, 's', 't', 's', 'z');
    put_u32(&w, 0);
    put_u32(&w, 4);
    put_u32(&w, 3);
    end_box(&w, sound_stsz);
    const u32 sound_patch = put_chunk_table(&w, spec.co64);
    end_box(&w, sound_table);
    end_box(&w, sound_info);
    end_box(&w, sound_media);
    end_box(&w, sound);
    if (!spec.aac) {
        // Without the AAC entry only the 'alac' alternate remains.
        w.bytes[mp4a + 4] = 'a';
        w.bytes[mp4a + 5] = 'l';
        w.bytes[mp4a + 6] = 'a';
        w.bytes[mp4a + 7] = 'c';
    }
    end_box(&w, moov);
    const u32 wide = begin_box(&w, 'w', 'i', 'd', 'e');
    end_box(&w, wide);
    // 64-bit mdat header, as large camera recordings write.
    const u32 mdat = w.count;
    put_u32(&w, 1);
    put_u32(&w, 0x6d646174u);
    put_u32(&w, 0);
    put_u32(&w, 0);
    const u32 video_chunk = w.count;
    for (u32 i = 0; i < 3; ++i) {
        put_u32(&w, 2);
        put_u8(&w, i ? u8{0x41} : u8{0x65});
        put_u8(&w, 0x88);
    }
    const u32 audio_chunk = w.count;
    for (u32 i = 0; i < 12; ++i)
        put_u8(&w, static_cast<u8>(0x21 + i));
    patch_u32(&w, mdat + 12, w.count - mdat);
    patch_u32(&w, video_patch + (spec.co64 ? 4u : 0u), video_chunk);
    patch_u32(&w, sound_patch + (spec.co64 ? 4u : 0u), audio_chunk);
    return w.ok;
}

int quicktime_layout_tests() noexcept {
    constexpr i32 one = 0x10000;
    if (mp4_matrix_rotation(one, 0, 0, one) != 0 || mp4_matrix_rotation(0, one, -one, 0) != 1 ||
        mp4_matrix_rotation(-one, 0, 0, -one) != 2 || mp4_matrix_rotation(0, -one, one, 0) != 3 ||
        mp4_matrix_rotation(0, 2 * one, -2 * one, 0) != 1 || mp4_matrix_rotation(-one, 0, 0, one) != 0 ||
        mp4_matrix_rotation(0, one, one, 0) != 0 || mp4_matrix_rotation(one, one, 0, one) != 0 ||
        mp4_matrix_rotation(0, 0, 0, 0) != 0)
        return 100;
    constexpr i32 matrices[4][4]{{one, 0, 0, one}, {0, one, -one, 0}, {-one, 0, 0, -one}, {0, -one, one, 0}};
    Writer w{};
    for (u32 variant = 0; variant < 8; ++variant) {
        MovieSpec spec{};
        for (u32 i = 0; i < 4; ++i)
            spec.matrix[i] = matrices[variant & 3][i];
        spec.co64 = (variant & 4) != 0;
        Mp4Demuxer video{};
        if (!movie_fixture(w, spec) || mp4_demux_initialize(&video, {w.bytes, w.count}) != Mp4DemuxError::none ||
            video.rotation != (variant & 3) || video.width != 32 || video.height != 16 ||
            video.sample_count != 3 || video.chunk_offsets_64 != spec.co64 || !video.has_edit_list ||
            video.duration_us != 300'000 || video.nal_length_bytes != 4 || hevc_container_present({w.bytes, w.count}))
            return 101;
        constexpr i64 presentation[]{0, 200'000, 100'000};
        for (u32 i = 0; i < 3; ++i) {
            const auto unit = mp4_demux_next(&video);
            if (!unit.ok() || unit.presentation_time_us_signed != presentation[i] || unit.sync != (i == 0) ||
                unit.bytes.count != 6 || unit.bytes[4] != (i ? 0x41 : 0x65))
                return 102;
        }
        const auto extent = video_display_rotated(video_display_extent(video.width, video.height, 1, 1),
                                                  video.rotation);
        if (extent.width != ((variant & 1) ? 16 : 32) || extent.height != ((variant & 1) ? 32 : 16))
            return 103;
        Mp4AudioDemuxer audio{};
        if (mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::none ||
            audio.sample_rate != 44100 || audio.channels != 2 || audio.samples.chunk_offsets_64 != spec.co64 ||
            audio.samples.media_start_ticks != 1024 || audio.samples.rotation)
            return 104;
        for (u32 i = 0; i < 3; ++i) {
            const auto unit = mp4_audio_demux_next(&audio);
            if (!unit.ok() || unit.bytes.count != 4 || unit.bytes[0] != 0x21 + 4 * i)
                return 105;
        }
    }
    // HEVC behind the same disabled alternate: container routing, demuxing and
    // rotation agree on the decodable track.
    MovieSpec hevc{};
    hevc.hevc = true;
    for (u32 i = 0; i < 4; ++i)
        hevc.matrix[i] = matrices[1][i];
    Mp4Demuxer d{};
    if (!movie_fixture(w, hevc) || !hevc_container_present({w.bytes, w.count}) ||
        hevc_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none || d.rotation != 1 ||
        hevc_demux_hvcc(d).count != 23 || d.width != 32 ||
        mp4_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::unsupported)
        return 106;
    // Only four zero bytes close an atom list; other trailing bytes are not atoms.
    MovieSpec tail{};
    tail.terminator = 1;
    if (!movie_fixture(w, tail) || mp4_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::malformed ||
        d.ready)
        return 107;
    hevc.terminator = 1;
    if (!movie_fixture(w, hevc) || hevc_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::malformed)
        return 108;
    // ISO layouts without terminator or alternates read the same tracks.
    MovieSpec plain{};
    plain.terminated = false;
    plain.alternates = false;
    Mp4AudioDemuxer audio{};
    if (!movie_fixture(w, plain) || mp4_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::none ||
        w.bytes[g_movie_stream_type] != 0x14)
        return 109;
    // Both low-bit spellings of the audio stream type decode; a visual stream does not.
    w.bytes[g_movie_stream_type] = 0x15;
    if (mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 111;
    w.bytes[g_movie_stream_type] = 0x11;
    if (mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::unsupported)
        return 112;
    // An ALAC-only movie has an audio track without a decoder: unsupported,
    // which playback treats as silent video (no_audio stays distinct).
    MovieSpec alac{};
    alac.aac = false;
    if (!movie_fixture(w, alac) || mp4_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        mp4_audio_demux_initialize(&audio, {w.bytes, w.count}) != Mp4DemuxError::unsupported)
        return 110;
    return 0;
}

int audio_tests() noexcept {
    Writer w{};
    Mp4AudioDemuxer d{};
    constexpr u32 rates[]{48000, 44100};
    constexpr u8 channel_counts[]{1, 2};
    for (u32 rate : rates)
        for (u8 channels : channel_counts) {
            if (!audio_fixture(w, rate, channels) ||
                mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
                d.sample_rate != rate || d.channels != channels || d.frames_per_packet != 1024 ||
                mp4_audio_demux_asc(d).count != 2 || d.samples.maximum_sample_bytes != 4)
                return 20;
            const auto a = mp4_audio_demux_next(&d), b = mp4_audio_demux_next(&d);
            if (!a.ok() || !b.ok() || a.bytes.data + 4 != b.bytes.data || a.decode_time_ticks != 0 ||
                b.decode_time_ticks != 1024 || a.presentation_time_us_signed != 0 ||
                b.presentation_time_ticks != 1024 || a.duration_ticks != 1024 || b.duration_ticks != 1024)
                return 21;
            Mp4SeekPoint point{};
            if (mp4_audio_demux_seek(&d, 30000, &point) != Mp4DemuxError::none || point.sample_index != 1 ||
                mp4_audio_demux_next(&d).decode_time_ticks != 1024)
                return 22;
            const auto saved = d;
            point = {77, 88, 99};
            if (mp4_audio_demux_seek(&d, -1, &point) != Mp4DemuxError::invalid_input ||
                !bytes_equal(&d, &saved, sizeof(d)) || point.sample_index != 77)
                return 23;
        }
    if (!audio_fixture(w, 48000, 2, 0, true) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 24;
    const auto preroll = mp4_audio_demux_next(&d), start = mp4_audio_demux_next(&d);
    if (preroll.presentation_time_us_signed != -21333 || preroll.presentation_time_us != 0 ||
        start.presentation_time_us_signed != 0 || d.samples.duration_us != 42667)
        return 25;
    if (!audio_fixture(w, 48000, 2, 4) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::unsupported || d.samples.ready)
        return 26;
    if (!audio_fixture(w) || mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 27;
    w.bytes[d.asc_offset] = 0x19;
    if (mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::unsupported)
        return 28;
    if (!audio_fixture(w) || mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 29;
    patch_u32(&w, d.samples.stts_offset, 4);
    if (mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::malformed)
        return 30;
    if (!audio_fixture(w) || mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none)
        return 31;
    patch_u32(&w, d.samples.chunk_offset_table, w.count - 10);
    if (mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::malformed)
        return 32;
    if (!fixture(&w) || mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::no_audio)
        return 33;
    return 0;
}

int exact_audio_time_tests() noexcept {
    Writer w{};
    Mp4AudioDemuxer d{};
    i64 time = 77;
    Mp4AudioSampleWindow window{};
    const u8 he44[]{0x2b, 0x92, 0x08, 0};
    if (!audio_fixture(w, 22050, 2, 0, false, 0, 0, 0, 1024, 0, {he44, 4}) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        d.sample_rate != 44100 || d.frames_per_packet != 2048 || d.samples.timescale != 22050 ||
        mp4_audio_demux_asc(d).count != 4 || !mp4_audio_demux_frame_time_100ns(d, 2048, &time) ||
        time != 464399 || !mp4_audio_demux_frame_time_100ns(d, 6144, &time) || time != 1393197 ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame ||
        window.end_source_frame != 6144 || window.duration_100ns != 1393197 ||
        window.leading_silence_frames)
        return 80;
    Mp4AudioFrameTimeBounds he_bounds{};
    if (!mp4_audio_demux_frame_time_bounds_100ns(d, 2048, &he_bounds) || he_bounds.floor_100ns != 464399 ||
        he_bounds.ceil_100ns != 464400)
        return 89;
    time = 77;
    const auto he_window = window;
    if (mp4_audio_demux_frame_time_100ns(d, 6145, &time) || time != 77 ||
        mp4_audio_demux_sample_window(d, 1393198, &window) ||
        !bytes_equal(&window, &he_window, sizeof(window)))
        return 81;
    patch_u32(&w, d.samples.stts_offset + 4, 2048);
    if (mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::unsupported || d.samples.ready)
        return 84;
    if (!audio_fixture(w, 22050, 2, 0, true, 0, 0, 0, 1024, 0, {he44, 4}) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        !mp4_audio_demux_frame_time_100ns(d, 0, &time) || time != -464400 ||
        !mp4_audio_demux_frame_time_100ns(d, 2048, &time) || time ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame != 2048 ||
        window.end_source_frame != 6144 || window.duration_100ns != 928798 || window.leading_silence_frames)
        return 82;
    if (!audio_fixture(w, 22050, 2, 0, true, 1001, 1, 41, 1, 0, {he44, 4}) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame != 2 ||
        window.end_source_frame != 1809 || window.leading_silence_frames != 45 ||
        window.duration_100ns != 419580)
        return 83;
    // A redundant zero-offset composition table is the identity mapping,
    // including signed version1/multiple validated runs. Nonzero offsets and
    // malformed sample-count totals retain explicit refusal.
    for (u8 kind = 1; kind <= 5; ++kind) {
        if (!audio_fixture(w, 48000, 2, 0, false, 0, 0, 0, 1024, kind))
            return 48;
        const auto e = mp4_audio_demux_initialize(&d, {w.bytes, w.count});
        if (kind == 1 || kind == 5) {
            if (e != Mp4DemuxError::none || mp4_audio_demux_next(&d).presentation_time_us_signed ||
                mp4_audio_demux_next(&d).presentation_time_ticks != 1024 ||
                !mp4_audio_demux_frame_time_100ns(d, 1024, &time) || time != 213333)
                return 49;
        } else if (e != Mp4DemuxError::unsupported || d.samples.ready)
            return 50;
    }
    patch_u32(&w, d.samples.ctts_offset, 4);
    if (mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::malformed)
        return 51;
    if (!audio_fixture(w) || mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        d.samples.movie_timescale != 48000 || d.samples.movie_duration_ticks != 3072 ||
        d.samples.empty_edit_ticks || !mp4_audio_demux_frame_time_100ns(d, 1, &time) || time != 208 ||
        !mp4_audio_demux_frame_time_100ns(d, 1024, &time) || time != 213333 ||
        !mp4_audio_demux_frame_time_100ns(d, 3072, &time) || time != 640000 ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame ||
        window.end_source_frame != 3072 || window.leading_silence_frames || window.duration_100ns != 640000)
        return 34;
    Mp4AudioFrameTimeBounds bounds{};
    if (!mp4_audio_demux_frame_time_bounds_100ns(d, 1, &bounds) || bounds.floor_100ns != 208 ||
        bounds.ceil_100ns != 209 || !mp4_audio_demux_frame_time_bounds_100ns(d, 3072, &bounds) ||
        bounds.floor_100ns != 640000 || bounds.ceil_100ns != 640000)
        return 85;
    const auto saved_bounds = bounds;
    if (mp4_audio_demux_frame_time_bounds_100ns(d, 3073, &bounds) ||
        !bytes_equal(&bounds, &saved_bounds, sizeof(bounds)) ||
        mp4_audio_demux_frame_time_bounds_100ns(d, 0, reinterpret_cast<Mp4AudioFrameTimeBounds*>(&d)))
        return 86;
    if (!mp4_audio_demux_sample_window(d, 1, &window) || window.first_source_frame != 1 ||
        window.end_source_frame != 3072 || window.leading_silence_frames != 1 ||
        !mp4_audio_demux_sample_window(d, 640000, &window) || window.first_source_frame != 3072 ||
        window.end_source_frame != 3072 || window.leading_silence_frames)
        return 35;
    const auto saved_window = window;
    time = 77;
    if (mp4_audio_demux_frame_time_100ns(d, 3073, &time) || time != 77 ||
        mp4_audio_demux_frame_time_100ns(d, 0, &d.samples.media_start_ticks) ||
        d.samples.media_start_ticks || mp4_audio_demux_sample_window(d, -1, &window) ||
        mp4_audio_demux_sample_window(d, 640001, &window) ||
        !bytes_equal(&window, &saved_window, sizeof(window)))
        return 36;
    if (!audio_fixture(w, 44100) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        !mp4_audio_demux_frame_time_100ns(d, 1024, &time) || time != 232199 ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.duration_100ns != 696598)
        return 37;
    // Final LC drain block from the controlled actual source: 419*1024 PCM
    // frames at 44100 Hz is 97291609 + 431/441 native ticks, not an integer.
    auto drain = d;
    drain.samples.sample_count = 420;
    if (!mp4_audio_demux_frame_time_bounds_100ns(drain, 429056, &bounds) ||
        bounds.floor_100ns != 97291609 || bounds.ceil_100ns != 97291610)
        return 87;
    // Negative native timestamps use floor, not truncation toward zero. The
    // retained source crop is exact1024, independent from rounded legacy us.
    if (!audio_fixture(w, 48000, 2, 0, true) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        !mp4_audio_demux_frame_time_100ns(d, 0, &time) || time != -213334 ||
        !mp4_audio_demux_frame_time_100ns(d, 1024, &time) || time ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame != 1024 ||
        window.end_source_frame != 3072 || window.leading_silence_frames || window.duration_100ns != 426666)
        return 38;
    if (!mp4_audio_demux_frame_time_bounds_100ns(d, 0, &bounds) || bounds.floor_100ns != -213334 ||
        bounds.ceil_100ns != -213333 || !mp4_audio_demux_frame_time_bounds_100ns(d, 1024, &bounds) ||
        bounds.floor_100ns || bounds.ceil_100ns)
        return 88;
    // Different movie/media clocks: nonintegral empty gap and edited endpoint
    // must not be converted through rounded microseconds for sample cropping.
    if (!audio_fixture(w, 48000, 2, 0, true, 1001, 1, 41, 1) ||
        mp4_audio_demux_initialize(&d, {w.bytes, w.count}) != Mp4DemuxError::none ||
        d.samples.movie_timescale != 1001 || d.samples.empty_edit_ticks != 1 ||
        d.samples.movie_duration_ticks != 42 || !mp4_audio_demux_frame_time_100ns(d, 0, &time) ||
        time != 9781 || !mp4_audio_demux_frame_time_100ns(d, 1, &time) || time != 9990 ||
        !mp4_audio_demux_sample_window(d, 0, &window) || window.first_source_frame != 1 ||
        window.end_source_frame != 1968 || window.leading_silence_frames != 48 ||
        window.duration_100ns != 419580 || !mp4_audio_demux_sample_window(d, 200000, &window) ||
        window.first_source_frame != 914 || window.end_source_frame != 1968 ||
        window.leading_silence_frames != 1)
        return 39;
    // Pure extreme-timebase arithmetic: the proper fraction requires a wide
    // intermediate, but our bounded modular path retains its negative floor.
    auto extreme = d;
    extreme.samples.timescale = 4294967250u;
    extreme.samples.movie_timescale = 4294967279u;
    extreme.samples.sample_count = 48;
    extreme.samples.media_duration_ticks = u64{48} * 91625968;
    extreme.samples.media_start_ticks = 4294967249ll;
    extreme.samples.empty_edit_ticks = 4294967277ull;
    extreme.samples.movie_duration_ticks = 4294967279ull;
    if (!mp4_audio_demux_frame_time_100ns(extreme, 0, &time) || time != -1)
        return 40;
    time = 77;
    window = saved_window;
    if (mp4_audio_demux_frame_time_100ns(extreme, 1, &time) || time != 77)
        return 41; // LCM exceeds u64.
    extreme.samples.movie_timescale = 1;
    extreme.samples.movie_duration_ticks = ~u64{0};
    if (mp4_audio_demux_sample_window(extreme, 0, &window) ||
        !bytes_equal(&window, &saved_window, sizeof(window)))
        return 42;
    return 0;
}

u64 scalar_scale_us_reference(u64 value, u32 timescale) noexcept {
    if (!timescale) return 0;
    const u64 seconds = value / timescale;
    const u64 remainder = value % timescale;
    if (seconds > ~u64{0} / 1000000) return ~u64{0};
    const u64 base = seconds * 1000000;
    const u64 fraction = (remainder * 1000000 + timescale / 2u) / timescale;
    return fraction > ~u64{0} - base ? ~u64{0} : base + fraction;
}
int scale_us_tests() noexcept {
    constexpr u64 bound = (~u64{0} - (~u32{0} / 2u)) / 1000000;
    const u64 values[] = {0, 1, 2, 3, 499, 500, 501, 999, 1000, 1001,
        0xffffffffull, 0x100000000ull, bound - 1, bound, bound + 1,
        ~u64{0} / 1000000 - 1, ~u64{0} / 1000000,
        ~u64{0} / 1000000 + 1, 0x7fffffffffffffffull,
        ~u64{0} - 1, ~u64{0}};
    const u32 scales[] = {0, 1, 2, 3, 7, 128, 256, 1000, 30000, 44100, 48000, 90000,
        999999, 1000000, 1000001, 1000000000, 0x7fffffff, 0xfffffffe, 0xffffffff};
    for (u64 value : values)
        for (u32 scale : scales)
            if (mp4_demux_detail::scale_us(value, scale) !=
                scalar_scale_us_reference(value, scale)) return 90;
    for (u32 scale : scales) {
        if (!scale || scale >= 1000000) continue;
        u64 lower = 0, upper = ~u64{0};
        while (lower < upper) {
            const u64 middle = lower + (upper - lower) / 2;
            if (scalar_scale_us_reference(middle, scale) == ~u64{0})
                upper = middle;
            else
                lower = middle + 1;
        }
        for (u64 value = lower - 2; value <= lower + 2; ++value)
            if (mp4_demux_detail::scale_us(value, scale) !=
                scalar_scale_us_reference(value, scale)) return 90;
    }
    u64 seed = 0x48b18368dc89ac01ull;
    const auto next = [&]() noexcept {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    for (u32 i = 0; i < 100000; ++i) {
        const u64 large = next(), small = next() % (bound + 2);
        const u32 scale = static_cast<u32>(next());
        if (mp4_demux_detail::scale_us(large, scale) !=
                scalar_scale_us_reference(large, scale) ||
            mp4_demux_detail::scale_us(small, scale) !=
                scalar_scale_us_reference(small, scale)) return 91;
    }
    return 0;
}

int hevc_timestamp_tests() noexcept {
    using namespace mp4_demux_detail;
    Writer w{};
    constexpr i32 offsets[]{-50, 0, 150};
    if (!fixture(&w, 0, 0, 0, 8, true, 0, -1, 3, ~u32{0}, 0, 0,
                 offsets, false, true)) return 92;
    const ConstSlice<u8> source{w.bytes, w.count};
    Box moov{}, track{}, media{}, header{};
    if (!find_box(source, 0, source.count, fourcc('m', 'o', 'o', 'v'), &moov) ||
        !find_box(source, moov.payload, moov.end, fourcc('t', 'r', 'a', 'k'), &track) ||
        !find_box(source, track.payload, track.end, fourcc('m', 'd', 'i', 'a'), &media) ||
        !find_box(source, media.payload, media.end, fourcc('m', 'd', 'h', 'd'), &header))
        return 92;
    patch_u32(&w, static_cast<u32>(header.payload) + 12, 90000);
    Mp4Demuxer d{};
    if (!hevc_container_present(source) || hevc_demux_initialize(&d, source) != Mp4DemuxError::none ||
        d.timescale != 90000 || d.sample_count != 3 || d.duration_us != 5000 ||
        hevc_demux_hvcc(d).count != 23) return 92;
    constexpr i64 expected[]{-556, 1111, 3889};
    for (u32 pass = 0; pass < 2; ++pass) {
        mp4_demux_reset(&d);
        for (u32 i = 0; i < 3; ++i) {
            const auto unit = mp4_demux_next(&d);
            if (!unit.ok() || unit.sample_index != i || unit.presentation_time_us_signed != expected[i] ||
                unit.decode_time_us != scalar_scale_us_reference(u64{i} * 100, 90000) ||
                unit.duration_us != 1111 || unit.presentation_time_ticks != i64{i} * 100 + offsets[i])
                return 93;
        }
        if (mp4_demux_next(&d).error != Mp4DemuxError::end_of_stream) return 93;
    }
    return 0;
}

int run() noexcept {
    const int scale_result = scale_us_tests();
    if (scale_result) return scale_result;
    const int hevc_result = hevc_timestamp_tests();
    if (hevc_result) return hevc_result;
    struct IntervalCase {
        i64 pts;
        u64 duration, end;
        bool edited;
        Mp4FrameVisibility visibility;
        u64 visible;
    };
    constexpr IntervalCase intervals[]{
        {-100, 100, 1000, true, Mp4FrameVisibility::before, 0},
        {-100, 80, 1000, true, Mp4FrameVisibility::before, 0},
        {-100, 120, 1000, true, Mp4FrameVisibility::visible, 20},
        {80, 50, 100, true, Mp4FrameVisibility::visible, 20},
        {100, 50, 100, true, Mp4FrameVisibility::after, 0},
        {120, 50, 100, true, Mp4FrameVisibility::after, 0},
        {80, 50, 100, false, Mp4FrameVisibility::visible, 50},
        {0, 0, 100, true, Mp4FrameVisibility::invalid, 0},
        {0x7fffffffffffffffll, 1, 100, false, Mp4FrameVisibility::invalid, 0},
        {-0x7fffffffffffffffll - 1, 0x7fffffffffffffffull, 100, true, Mp4FrameVisibility::before, 0},
        {-5, 10, ~u64{0}, true, Mp4FrameVisibility::invalid, 0},
        {-5, 10, 3, true, Mp4FrameVisibility::visible, 3}};
    for (const auto& spec : intervals) {
        const auto value = mp4_frame_interval(spec.pts, spec.duration, spec.end, spec.edited);
        if (value.visibility != spec.visibility || value.visible_duration_us != spec.visible)
            return 16;
    }
    if (mp4_frame_interval_from(25, 10, 35, 100, true).visibility != Mp4FrameVisibility::before ||
        mp4_frame_interval_from(25, 15, 35, 100, true).visible_duration_us != 5 ||
        mp4_frame_interval_from(-100, 120, 5, 100, true).visible_duration_us != 15 ||
        mp4_frame_interval_from(80, 50, 90, 100, true).visible_duration_us != 10 ||
        mp4_frame_interval_from(0, 10, -1, 100, true).visibility != Mp4FrameVisibility::invalid)
        return 43;
    Writer writer{};
    if (!fixture(&writer)) {
        return 1;
    }
    Mp4Demuxer demuxer{};
    if (mp4_demux_initialize(&demuxer, ConstSlice<u8>{writer.bytes, writer.count}) != Mp4DemuxError::none ||
        demuxer.width != 16 || demuxer.height != 16 || demuxer.sar_width || demuxer.sar_height ||
        demuxer.sample_count != 1 || demuxer.nal_length_bytes != 4 || mp4_demux_avcc(demuxer).count != 15) {
        return 2;
    }
    const Mp4AccessUnit unit = mp4_demux_next(&demuxer);
    if (!unit.ok() || unit.bytes.count != 6 || unit.bytes.data[4] != 0x65 || unit.bytes.data[5] != 0x88 ||
        unit.duration_us != 100'000 || unit.presentation_time_us != 0 || !unit.sync) {
        return 3;
    }
    if (mp4_demux_next(&demuxer).error != Mp4DemuxError::end_of_stream) {
        return 4;
    }
    mp4_demux_reset(&demuxer);
    if (!mp4_demux_next(&demuxer).ok()) {
        return 5;
    }
    Mp4SeekPoint seek{};
    if (mp4_demux_seek_sync(&demuxer, 0, &seek) != Mp4DemuxError::none || seek.sample_index != 0 ||
        !mp4_demux_next(&demuxer).ok())
        return 11;
    if (!fixture(&writer, 0, 0, 0, 8, true, -20) ||
        mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
        mp4_demux_next(&demuxer).presentation_time_us_signed != -20000)
        return 12;
    if (!fixture(&writer, 0, 0, 0, 8, false, 0, 20) ||
        mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
        demuxer.duration_us != 80000 || mp4_demux_next(&demuxer).presentation_time_us_signed != -20000)
        return 13;
    if (!fixture(&writer) ||
        mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none)
        return 14;
    const auto valid = mp4_demux_next(&demuxer);
    writer.bytes[static_cast<u32>(valid.bytes.data - writer.bytes) + 4] = 0x41;
    const auto saved = demuxer;
    seek = {77, 88, 99};
    if (mp4_demux_seek_sync(&demuxer, 0, &seek) != Mp4DemuxError::unsupported ||
        !bytes_equal(&demuxer, &saved, sizeof(saved)) || seek.sample_index != 77)
        return 15;
    if (!fixture(&writer, 0, 0, 0, 8, true, -20, -1, 5, 3) ||
        mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none)
        return 44;
    const i64 targets[]{0, 279999, 280000, 500000};
    const u32 expected[]{0, 0, 3, 3};
    for (u32 i = 0; i < 4; ++i) {
        if (mp4_demux_seek_sync(&demuxer, targets[i], &seek) != Mp4DemuxError::none ||
            seek.sample_index != expected[i] ||
            seek.presentation_time_us != (expected[i] ? 280000 : -20000) ||
            mp4_demux_next(&demuxer).sample_index != expected[i])
            return 45;
    }
    const auto seek_saved = demuxer;
    seek = {77, 88, 99};
    if (mp4_demux_seek_sync(&demuxer, -1, &seek) != Mp4DemuxError::invalid_input ||
        mp4_demux_seek_sync(&demuxer, 500001, &seek) != Mp4DemuxError::invalid_input ||
        !bytes_equal(&demuxer, &seek_saved, sizeof(demuxer)) || seek.sample_index != 77)
        return 46;
    if (!fixture(&writer) ||
        mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none)
        return 47;
    writer.bytes[20] = 0;
    writer.bytes[21] = 0;
    writer.bytes[22] = 0;
    writer.bytes[23] = 4;
    if (mp4_demux_initialize(&demuxer, ConstSlice<u8>{writer.bytes, writer.count}) == Mp4DemuxError::none) {
        return 6;
    }
    const u32 ratios[][2]{{4, 3}, {1, 2}, {1, 1}, {0xFFFFFFFEu, 0xFFFFFFFFu}};
    for (const auto& ratio : ratios) {
        if (!fixture(&writer, ratio[0], ratio[1], 1) ||
            mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::none ||
            demuxer.sar_width != ratio[0] || demuxer.sar_height != ratio[1] || demuxer.width != 16 ||
            demuxer.height != 16)
            return 7;
        mp4_demux_reset(&demuxer);
        if (!mp4_demux_next(&demuxer).ok() || demuxer.sar_width != ratio[0])
            return 8;
    }
    const u32 invalid[][4]{{0, 1, 1, 8}, {1, 0, 1, 8}, {1, 1, 2, 8}, {1, 1, 1, 4}, {1, 1, 1, 12}};
    for (const auto& spec : invalid) {
        if (!fixture(&writer, spec[0], spec[1], spec[2], spec[3]) ||
            mp4_demux_initialize(&demuxer, {writer.bytes, writer.count}) != Mp4DemuxError::malformed ||
            demuxer.ready)
            return 9;
    }
    // Natural size grows the appropriate axis, while decoded dimensions
    // remain unchanged. Include maximum 32-bit pasp values and overflow.
    const u32 sizes[][6]{{66, 50, 4, 3, 88, 50},
                         {66, 50, 1, 2, 66, 100},
                         {720, 576, 64, 45, 1024, 576},
                         {720, 480, 8, 9, 720, 540},
                         {66, 50, 1, 1, 66, 50},
                         {1920, 1080, 297, 296, 1926, 1080},
                         {65535, 65535, 0xFFFFFFFFu, 0xFFFFFFFFu, 65535, 65535},
                         {2048, 2048, 0xFFFFFFFFu, 1, 0, 0},
                         {2048, 2048, 1, 0xFFFFFFFFu, 0, 0},
                         {0, 50, 1, 1, 0, 0},
                         {66, 0, 1, 1, 0, 0},
                         {66, 50, 1, 0, 0, 0}};
    for (const auto& size : sizes) {
        const auto extent = video_display_extent(size[0], size[1], size[2], size[3]);
        if (extent.width != size[4] || extent.height != size[5])
            return 10;
    }
    const int composition_result = composition_edit_tests();
    if (composition_result)
        return composition_result;
    const int audio_result = audio_tests();
    if (audio_result)
        return audio_result;
    const int quicktime_result = quicktime_tests();
    if (quicktime_result)
        return quicktime_result;
    const int layout_result = quicktime_layout_tests();
    return layout_result ? layout_result : exact_audio_time_tests();

}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_mp4_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
