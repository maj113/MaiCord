#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/media/hevc.hpp"
#include "maicord/core/byte_range.hpp"
#include "mp4_demux_detail.hpp"

namespace maicord {
namespace {
using namespace mp4_demux_detail;
bool hevc_entry(const Box& entry) noexcept {
    return entry.type == fourcc('h', 'v', 'c', '1') || entry.type == fourcc('h', 'e', 'v', '1');
}
bool visual_entry(ConstSlice<u8> source, const Box& stsd, Box* entry) noexcept {
    return stsd.end - stsd.payload >= 16 && read_u32(source.data + stsd.payload) == 0 &&
           first_description(source, stsd, entry);
}
} // namespace
bool hevc_container_present(ConstSlice<u8> source) noexcept {
    if (!source.data || source.count < 16 || source.count > kMediaMaximumProgressiveVideoBytes ||
        !byte_range_valid(source))
        return false;
    // The same track the demuxers select, so an H.264 and an HEVC track in
    // one movie cannot route the decoder to the other one's table.
    Box moov{}, track{}, mdia{}, minf{}, stbl{}, stsd{}, entry{};
    return find_box(source, 0, source.count, fourcc('m', 'o', 'o', 'v'), &moov) &&
           select_track(source, moov, fourcc('v', 'i', 'd', 'e'), &track) == Mp4DemuxError::none &&
           find_box(source, track.payload, track.end, fourcc('m', 'd', 'i', 'a'), &mdia) &&
           find_box(source, mdia.payload, mdia.end, fourcc('m', 'i', 'n', 'f'), &minf) &&
           find_box(source, minf.payload, minf.end, fourcc('s', 't', 'b', 'l'), &stbl) &&
           find_box(source, stbl.payload, stbl.end, fourcc('s', 't', 's', 'd'), &stsd) &&
           visual_entry(source, stsd, &entry) && hevc_entry(entry);
}
Mp4DemuxError hevc_demux_initialize(Mp4Demuxer* output, ConstSlice<u8> source,
                                    Mp4SourceAvailability availability) noexcept {
    if (!output || !byte_range_valid({reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        !byte_range_valid(source) ||
        byte_ranges_overlap(source, {reinterpret_cast<const u8*>(output), sizeof(*output)}))
        return Mp4DemuxError::invalid_input;
    Mp4Demuxer d{};
    Box description{};
    const auto e = initialize_tables(&d, source, fourcc('v', 'i', 'd', 'e'), &description, availability);
    if (e != Mp4DemuxError::none)
        return e;
    Box entry{}, config{};
    if (!visual_entry(source, description, &entry))
        return Mp4DemuxError::malformed;
    if (!hevc_entry(entry))
        return Mp4DemuxError::unsupported;
    if (entry.end - entry.payload < 78)
        return Mp4DemuxError::malformed;
    const auto* visual = source.data + entry.payload;
    d.width = (u32{visual[24]} << 8) | visual[25];
    d.height = (u32{visual[26]} << 8) | visual[27];
    if (!d.width || !d.height || d.width > 4096 || d.height > 4096 ||
        u64{d.width} * d.height > 2048u * 2048u)
        return Mp4DemuxError::unsupported;
    u64 cursor = entry.payload + 78;
    bool found{};
    // colr/btrt/dvvC and QuickTime's zero terminator are ignorable.
    while (!atom_list_end(source, cursor, entry.end)) {
        Box b{};
        if (!next_box(source, &cursor, entry.end, &b))
            return Mp4DemuxError::malformed;
        if (b.type == fourcc('h', 'v', 'c', 'C')) {
            if (found || b.end - b.payload < 23 || b.end - b.payload > 65536)
                return Mp4DemuxError::malformed;
            config = b;
            found = true;
        } else if (b.type == fourcc('p', 'a', 's', 'p')) {
            if (d.sar_width || b.end - b.payload != 8)
                return Mp4DemuxError::malformed;
            d.sar_width = read_u32(source.data + b.payload);
            d.sar_height = read_u32(source.data + b.payload + 4);
            if (!d.sar_width || !d.sar_height)
                return Mp4DemuxError::malformed;
        }
    }
    if (!found || source.data[config.payload] != 1)
        return Mp4DemuxError::malformed;
    d.avcc_offset = static_cast<u32>(config.payload);
    d.avcc_bytes = static_cast<u32>(config.end - config.payload);
    d.nal_length_bytes = static_cast<u8>((source.data[config.payload + 21] & 3u) + 1);
    if (d.nal_length_bytes == 3)
        return Mp4DemuxError::unsupported;
    *output = d;
    return Mp4DemuxError::none;
}
ConstSlice<u8> hevc_demux_hvcc(const Mp4Demuxer& d) noexcept {
    return d.ready && d.avcc_offset <= d.encoded.count && d.avcc_bytes <= d.encoded.count - d.avcc_offset
               ? ConstSlice<u8>{d.encoded.data + d.avcc_offset, d.avcc_bytes}
               : ConstSlice<u8>{};
}
Mp4DemuxError hevc_demux_seek_sync(Mp4Demuxer* output, i64 target, Mp4SeekPoint* point) noexcept {
    if (!output || !point || !output->ready || target < 0 || static_cast<u64>(target) > output->duration_us)
        return Mp4DemuxError::invalid_input;
    const ConstSlice<u8> result_span{reinterpret_cast<const u8*>(point), sizeof(*point)},
        owner_span{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(result_span) || byte_ranges_overlap(result_span, owner_span) ||
        byte_ranges_overlap(result_span, output->encoded))
        return Mp4DemuxError::invalid_input;
    return seek_sync(output, target, point, [](ConstSlice<u8> bytes, u8 length_bytes) noexcept {
        HevcNalCursor cursor{bytes, 0, length_bytes};
        bool idr{}, other{};
        u32 nals{};
        for (;;) {
            const auto nal = hevc_next_nal(&cursor);
            if (nal.error == HevcError::end)
                break;
            if (nal.error != HevcError::none)
                return SyncKind::malformed;
            if (++nals > 256)
                return SyncKind::unsupported;
            if (nal.type == 19 || nal.type == 20)
                idr = true;
            else if (nal.type < 32)
                other = true;
        }
        return idr && !other ? SyncKind::random_access : SyncKind::other;
    });
}
} // namespace maicord
