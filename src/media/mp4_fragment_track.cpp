#include "maicord/media/mp4_fragment.hpp"
#include "maicord/media/mp4_audio_demux.hpp"
#include "maicord/core/byte_range.hpp"
#include "mp4_demux_detail.hpp"

namespace maicord {
namespace {
using namespace mp4_demux_detail;
using E = Mp4DemuxError;
constexpr u64 limit = 0x7fffffffffffffffull;
// Check the complete child list, including duplicates and trailing truncation.
bool unique(ConstSlice<u8> bytes, const Box& parent, u32 type, Box* output, bool required = true) noexcept {
    u64 cursor = parent.payload;
    Box found{};
    while (cursor < parent.end) {
        Box child{};
        if (!next_box(bytes, &cursor, parent.end, &child)) return false;
        if (child.type == type) {
            if (found.end) return false;
            found = child;
        }
    }
    *output = found;
    return found.end || !required;
}
bool brand(u32 value) noexcept {
    return value == fourcc('i','s','o','m') || value == fourcc('d','a','s','h') ||
        value == fourcc('a','v','c','1') || value == fourcc('m','p','4','1') || value == fourcc('m','p','4','2') ||
        value == fourcc('c','m','f','c') || value == fourcc('c','m','f','s') ||
        ((value & 0xffffff00u) == fourcc('i','s','o',0) && (value & 255) >= '2' && (value & 255) <= '9');
}
bool clock(ConstSlice<u8> bytes, const Box& box, u32* timescale, u64* duration, bool movie) noexcept {
    if (box.end - box.payload < 4) return false;
    const auto* p = bytes.data + box.payload;
    const u32 version = read_u32(p);
    if ((version != 0 && version != 0x01000000) ||
        box.end - box.payload < (movie ? (version ? 112u : 100u) : (version ? 36u : 24u))) return false;
    *timescale = read_u32(p + (version ? 20 : 12));
    *duration = version ? read_u64(p + 24) : read_u32(p + 16);
    if (*duration == (version ? ~u64{0} : ~u32{0})) *duration = 0;
    return *timescale && *duration <= limit;
}
bool empty_table(ConstSlice<u8> bytes, const Box& parent, u32 type, u32 length, bool required = true) noexcept {
    Box table{};
    if (!unique(bytes, parent, type, &table, required)) return false;
    if (!table.end) return true;
    if (table.end - table.payload != length) return false;
    for (u32 i = 0; i < length; i += 4) if (read_u32(bytes.data + table.payload + i)) return false;
    return true;
}
E edits(ConstSlice<u8> bytes, const Box& track, Mp4FragmentTrack& out) noexcept {
    Box edts{}, elst{};
    if (!unique(bytes, track, fourcc('e','d','t','s'), &edts, false)) return E::malformed;
    if (!edts.end) return E::none;
    if (!unique(bytes, edts, fourcc('e','l','s','t'), &elst) || elst.end - elst.payload < 8) return E::malformed;
    const auto* p = bytes.data + elst.payload;
    const u32 version = read_u32(p), count = read_u32(p + 4), size = version ? 20 : 12;
    if (version != 0 && version != 0x01000000) return E::unsupported;
    if (!count || count > 2) return E::unsupported;
    if (elst.end - elst.payload != 8 + count * size) return E::malformed;
    p += 8;
    for (u32 i = 0; i < count; ++i, p += size) {
        const u64 span = version ? read_u64(p) : read_u32(p);
        const i64 media = version ? static_cast<i64>(read_u64(p + 8)) : static_cast<i32>(read_u32(p + 4));
        if (read_u32(p + size - 4) != 0x10000 || span > limit) return E::unsupported;
        if (count == 2 && !i) {
            if (media != -1 || !span) return E::unsupported;
            out.empty_edit = span;
        } else {
            if (media < 0 || span > limit - out.empty_edit) return E::unsupported;
            out.media_start = media; out.edit_duration = span;
        }
    }
    out.edited = true;
    return E::none;
}
} // namespace
Mp4DemuxError mp4_fragment_track_initialize(ConstSlice<u8> bytes, bool audio, Mp4FragmentTrack* output) noexcept {
    const ConstSlice<u8> region{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!output || !byte_range_valid(region) || !bytes.data || !byte_range_valid(bytes) ||
        byte_ranges_overlap(bytes, region) || bytes.count > 1024 * 1024) return E::invalid_input;
    const Box root{0, 0, 0, bytes.count};
    Box ftyp{}, moov{}, mvhd{}, mvex{}, track{};
    if (!unique(bytes, root, fourcc('f','t','y','p'), &ftyp) || ftyp.end - ftyp.payload < 8 ||
        (ftyp.end - ftyp.payload) % 4 || !unique(bytes, root, fourcc('m','o','o','v'), &moov)) return E::malformed;
    bool known_brand = brand(read_u32(bytes.data + ftyp.payload));
    for (u64 at = ftyp.payload + 8; at < ftyp.end; at += 4) known_brand |= brand(read_u32(bytes.data + at));
    if (!known_brand) return E::unsupported;
    u64 at = 0;
    while (at < bytes.count) {
        Box child{};
        if (!next_box(bytes, &at, bytes.count, &child)) return E::malformed;
        if (child.type == fourcc('m','d','a','t') || child.type == fourcc('m','o','o','f')) return E::unsupported;
    }
    if (!unique(bytes, moov, fourcc('m','v','h','d'), &mvhd) ||
        !unique(bytes, moov, fourcc('m','v','e','x'), &mvex)) return E::malformed;
    Mp4FragmentTrack value{}; value.audio = audio;
    u64 movie_duration{};
    if (!clock(bytes, mvhd, &value.movie_timescale, &movie_duration, true)) return E::malformed;
    const auto selected = select_track(bytes, moov, audio ? fourcc('s','o','u','n') : fourcc('v','i','d','e'), &track);
    if (selected != E::none) return selected;
    Box tkhd{}, mdia{}, mdhd{}, hdlr{}, minf{}, stbl{}, stsd{}, dinf{}, dref{}, entry{}, stco{}, co64{};
    if (!unique(bytes, track, fourcc('t','k','h','d'), &tkhd) || tkhd.end - tkhd.payload < 4 ||
        !unique(bytes, track, fourcc('m','d','i','a'), &mdia) ||
        !unique(bytes, mdia, fourcc('h','d','l','r'), &hdlr) || hdlr.end - hdlr.payload < 24 ||
        !unique(bytes, mdia, fourcc('m','d','h','d'), &mdhd) ||
        !unique(bytes, mdia, fourcc('m','i','n','f'), &minf) ||
        !unique(bytes, minf, fourcc('s','t','b','l'), &stbl) ||
        !unique(bytes, stbl, fourcc('s','t','s','d'), &stsd)) return E::malformed;
    const auto* header = bytes.data + tkhd.payload;
    const u32 version = header[0];
    if (version > 1 || (read_u32(header) & 0xfffff8u) || tkhd.end - tkhd.payload < (version ? 96u : 84u))
        return E::unsupported;
    const u32 track_id = read_u32(header + (version ? 20 : 12));
    if (!track_id || read_u32(bytes.data + hdlr.payload) ||
        read_u32(bytes.data + hdlr.payload + 8) != (audio ? fourcc('s','o','u','n') : fourcc('v','i','d','e')) ||
        !clock(bytes, mdhd, &value.timescale, &value.media_duration, false)) return E::malformed;
    u32 track_ids[32]{}, tracks = 0;
    at = moov.payload;
    while (at < moov.end) {
        Box child{}, id_box{};
        if (!next_box(bytes, &at, moov.end, &child)) return E::malformed;
        if (child.type != fourcc('t','r','a','k')) continue;
        if (tracks == 32) return E::unsupported;
        if (!unique(bytes, child, fourcc('t','k','h','d'), &id_box) || id_box.end - id_box.payload < 4) return E::malformed;
        const auto* p = bytes.data + id_box.payload;
        const u32 id_offset = p[0] == 1 ? 20 : 12;
        if (p[0] > 1 || id_box.end - id_box.payload < id_offset + 4) return E::malformed;
        const u32 id = read_u32(p + id_offset);
        if (!id) return E::malformed;
        for (u32 i = 0; i < tracks; ++i) if (track_ids[i] == id) return E::malformed;
        track_ids[tracks++] = id;
    }
    const auto* matrix = header + (version ? 52 : 40);
    value.rotation = mp4_matrix_rotation(static_cast<i32>(read_u32(matrix)), static_cast<i32>(read_u32(matrix + 4)),
        static_cast<i32>(read_u32(matrix + 12)), static_cast<i32>(read_u32(matrix + 16)));
    bool found = false;
    at = mvex.payload;
    while (at < mvex.end) {
        Box child{};
        if (!next_box(bytes, &at, mvex.end, &child)) return E::malformed;
        if (child.type != fourcc('t','r','e','x')) continue;
        if (child.end - child.payload != 24) return E::malformed;
        if (read_u32(bytes.data + child.payload + 4) != track_id) continue;
        if (found) return E::malformed;
        const auto parsed = mp4_fragment_defaults({bytes.data + child.begin, static_cast<u32>(child.end - child.begin)}, &value.defaults);
        if (parsed != E::none) return parsed;
        found = true;
    }
    if (!found) return E::malformed;
    if (!empty_table(bytes, stbl, fourcc('s','t','t','s'), 8) || !empty_table(bytes, stbl, fourcc('s','t','s','c'), 8) ||
        !empty_table(bytes, stbl, fourcc('s','t','s','z'), 12) ||
        !empty_table(bytes, stbl, fourcc('c','t','t','s'), 8, false) ||
        !empty_table(bytes, stbl, fourcc('s','t','s','s'), 8, false) ||
        !unique(bytes, stbl, fourcc('s','t','c','o'), &stco, false) ||
        !unique(bytes, stbl, fourcc('c','o','6','4'), &co64, false) || ((stco.end != 0) == (co64.end != 0)) ||
        !empty_table(bytes, stbl, stco.end ? fourcc('s','t','c','o') : fourcc('c','o','6','4'), 8)) return E::malformed;
    if (!unique(bytes, minf, fourcc('d','i','n','f'), &dinf) ||
        !unique(bytes, dinf, fourcc('d','r','e','f'), &dref) || dref.end - dref.payload < 8) return E::malformed;
    const auto* reference = bytes.data + dref.payload;
    if (read_u32(reference) || read_u32(reference + 4) != 1) return E::unsupported;
    at = dref.payload + 8;
    if (!next_box(bytes, &at, dref.end, &entry) || at != dref.end || entry.type != fourcc('u','r','l',' ') ||
        entry.end - entry.payload != 4 || read_u32(bytes.data + entry.payload) != 1) return E::unsupported;
    if (stsd.end - stsd.payload < 8 || read_u32(bytes.data + stsd.payload) ||
        read_u32(bytes.data + stsd.payload + 4) != 1 || !first_description(bytes, stsd, &entry) || entry.end != stsd.end)
        return E::malformed;
    if (audio ? entry.type != fourcc('m','p','4','a') :
        entry.type != fourcc('a','v','c','1') && entry.type != fourcc('a','v','c','3')) return E::unsupported;
    const u32 entry_header = audio ? 28 : 78;
    if (entry.end - entry.payload < entry_header || byte_load_be16(bytes.data + entry.payload + 6) != 1 ||
        (audio && byte_load_be16(bytes.data + entry.payload + 8))) return E::unsupported;
    at = entry.payload + entry_header;
    u32 configurations = 0;
    while (at < entry.end) {
        Box child{};
        if (!next_box(bytes, &at, entry.end, &child)) return E::malformed;
        if (child.type == fourcc('s','i','n','f') || child.type == fourcc('t','e','n','c') ||
            child.type == fourcc('u','u','i','d') || child.type == fourcc('w','a','v','e')) return E::unsupported;
        if (child.type == (audio ? fourcc('e','s','d','s') : fourcc('a','v','c','C'))) ++configurations;
    }
    if (configurations != 1) return E::malformed;
    Mp4AudioDemuxer codec{}; codec.samples.encoded = bytes; codec.samples.timescale = value.timescale;
    const auto decoded = audio ? parse_audio_description(codec, stsd) : parse_description(&codec.samples, stsd);
    if (decoded != E::none) return decoded;
    if (audio) {
        value.configuration = {bytes.data + codec.asc_offset, codec.asc_bytes};
        value.sample_rate = codec.sample_rate; value.channels = codec.channels; value.frames_per_packet = codec.frames_per_packet;
    } else {
        if (codec.samples.width > 2048 || codec.samples.height > 2048) return E::unsupported;
        value.configuration = {bytes.data + codec.samples.avcc_offset, codec.samples.avcc_bytes};
        value.width = codec.samples.width; value.height = codec.samples.height;
        value.sar_width = codec.samples.sar_width; value.sar_height = codec.samples.sar_height;
        value.nal_length_bytes = codec.samples.nal_length_bytes;
    }
    const auto edited = edits(bytes, track, value);
    if (edited != E::none) return edited;
    *output = value;
    return E::none;
}
} // namespace maicord
