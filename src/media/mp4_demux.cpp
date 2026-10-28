#include "maicord/media/mp4_demux.hpp"
#include "maicord/core/memory.hpp"
#include "mp4_demux_detail.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace mp4_demux_detail {

bool next_box(const ConstSlice<u8>& encoded, u64* cursor, u64 end,
              Box* output) noexcept {
    if (!cursor || !output || *cursor > end || end > encoded.count ||
        end - *cursor < 8u) {
        return false;
    }
    const u64 begin = *cursor;
    const u8* header = encoded.data + begin;
    u64 bytes = read_u32(header);
    u64 header_bytes = 8;
    if (bytes == 1) {
        if (end - begin < 16u) {
            return false;
        }
        bytes = read_u64(header + 8);
        header_bytes = 16;
    } else if (bytes == 0) {
        bytes = end - begin;
    }
    if (bytes < header_bytes || bytes > end - begin) {
        return false;
    }
    *output = Box{read_u32(header + 4), begin, begin + header_bytes,
                  begin + bytes};
    *cursor = begin + bytes;
    return true;
}

bool find_box(const ConstSlice<u8>& encoded, u64 begin, u64 end, u32 type,
              Box* output) noexcept {
    u64 cursor = begin;
    Box box{};
    while (cursor < end && next_box(encoded, &cursor, end, &box)) {
        if (box.type == type) {
            *output = box;
            return true;
        }
    }
    return false;
}

bool atom_list_end(const ConstSlice<u8>& encoded, u64 cursor, u64 end) noexcept {
    return cursor == end || (cursor < end && end - cursor == 4u && end <= encoded.count &&
                             read_u32(encoded.data + cursor) == 0);
}

bool first_description(const ConstSlice<u8>& encoded, const Box& stsd, Box* entry) noexcept {
    if (stsd.end - stsd.payload < 16u || !read_u32(encoded.data + stsd.payload + 4u))
        return false;
    u64 cursor = stsd.payload + 8u;
    return next_box(encoded, &cursor, stsd.end, entry);
}

bool table(const ConstSlice<u8>& encoded, const Box& box, u32 entry_bytes,
           u32* offset, u32* count) noexcept {
    if (box.end - box.payload < 8u) {
        return false;
    }
    const u32 version_flags=read_u32(encoded.data+box.payload);
    if ((version_flags&0x00ffffffu) || (box.type==fourcc('c','t','t','s')?
        version_flags>0x01000000u:version_flags!=0)) return false;
    const u32 entries = read_u32(encoded.data + box.payload + 4u);
    const u64 first = box.payload + 8u;
    if (static_cast<u64>(entries) * entry_bytes > box.end - first || first > ~0u) {
        return false;
    }
    *offset = static_cast<u32>(first);
    *count = entries;
    return true;
}

u64 scale_us(u64 value, u32 timescale) noexcept {
    if (timescale == 0) {
        return 0;
    }
    // Reserve the largest u32 half-timescale before the multiply.
    constexpr u64 maximum_direct_value =
        (~u64{0} - (~u32{0} / 2u)) / 1'000'000ull;
    if (value <= maximum_direct_value)
        return (value * 1'000'000ull + timescale / 2u) / timescale;
    const u64 seconds = value / timescale;
    const u64 remainder = value % timescale;
    if (seconds > (~0ull / 1'000'000ull)) {
        return ~0ull;
    }
    const u64 base=seconds*1'000'000ull;
    const u64 fraction=(remainder*1'000'000ull+timescale/2u)/timescale;
    return fraction>~0ull-base?~0ull:base+fraction;
}

bool media_track(const ConstSlice<u8>& encoded, const Box& track, u32 handler,
                 Box* media, Box* sample_table) noexcept {
    Box mdia{};
    Box hdlr{};
    Box minf{};
    Box stbl{};
    if (!find_box(encoded, track.payload, track.end,
                  fourcc('m', 'd', 'i', 'a'), &mdia) ||
        !find_box(encoded, mdia.payload, mdia.end,
                  fourcc('h', 'd', 'l', 'r'), &hdlr) ||
        hdlr.end - hdlr.payload < 12u ||
        read_u32(encoded.data + hdlr.payload + 8u) !=
            handler ||
        !find_box(encoded, mdia.payload, mdia.end,
                  fourcc('m', 'i', 'n', 'f'), &minf) ||
        !find_box(encoded, minf.payload, minf.end,
                  fourcc('s', 't', 'b', 'l'), &stbl)) {
        return false;
    }
    *media = mdia;
    *sample_table = stbl;
    return true;
}

bool known_description(const ConstSlice<u8>& encoded, const Box& sample_table, u32 handler) noexcept {
    Box stsd{}, entry{};
    if (!find_box(encoded, sample_table.payload, sample_table.end, fourcc('s', 't', 's', 'd'), &stsd) ||
        !first_description(encoded, stsd, &entry))
        return false;
    if (handler == fourcc('s', 'o', 'u', 'n'))
        return entry.type == fourcc('m', 'p', '4', 'a');
    return entry.type == fourcc('a', 'v', 'c', '1') || entry.type == fourcc('a', 'v', 'c', '3') ||
           entry.type == fourcc('h', 'v', 'c', '1') || entry.type == fourcc('h', 'e', 'v', '1');
}

// The tkhd flags/matrix. An absent header reads as an enabled, unrotated track.
bool track_header(const ConstSlice<u8>& encoded, const Box& track, bool* enabled, u8* rotation) noexcept {
    Box tkhd{};
    *enabled = true;
    *rotation = 0;
    if (!find_box(encoded, track.payload, track.end, fourcc('t', 'k', 'h', 'd'), &tkhd))
        return true;
    if (tkhd.end - tkhd.payload < 4u)
        return false;
    *enabled = (read_u32(encoded.data + tkhd.payload) & 1u) != 0;
    const u8 version = encoded.data[tkhd.payload];
    const u32 matrix = version == 1 ? 52u : 40u;
    if (version > 1 || tkhd.end - tkhd.payload < matrix + 36u)
        return true;
    const u8* m = encoded.data + tkhd.payload + matrix;
    *rotation = mp4_matrix_rotation(static_cast<i32>(read_u32(m)), static_cast<i32>(read_u32(m + 4u)),
                                    static_cast<i32>(read_u32(m + 12u)), static_cast<i32>(read_u32(m + 16u)));
    return true;
}

Mp4DemuxError select_track(const ConstSlice<u8>& encoded, const Box& moov, u32 handler, Box* selected) noexcept {
    u64 cursor = moov.payload;
    u32 best = 0;
    while (cursor < moov.end) {
        Box track{}, media{}, header{}, sample_table{};
        if (!next_box(encoded, &cursor, moov.end, &track))
            return Mp4DemuxError::malformed;
        if (track.type != fourcc('t', 'r', 'a', 'k'))
            continue;
        // Metadata ('meta'/mebx), timecode ('tmcd') and other tracks are skipped
        // by handler; every track must still carry one.
        if (!find_box(encoded, track.payload, track.end, fourcc('m', 'd', 'i', 'a'), &media) ||
            !find_box(encoded, media.payload, media.end, fourcc('h', 'd', 'l', 'r'), &header) ||
            header.end - header.payload < 12u)
            return Mp4DemuxError::malformed;
        if (read_u32(encoded.data + header.payload + 8u) != handler)
            continue;
        bool enabled{};
        u8 rotation{};
        if (!media_track(encoded, track, handler, &media, &sample_table) ||
            !track_header(encoded, track, &enabled, &rotation))
            return Mp4DemuxError::malformed;
        const u32 rank = 1u + (known_description(encoded, sample_table, handler) ? 2u : 0u) + (enabled ? 1u : 0u);
        if (rank > best) {
            best = rank;
            *selected = track;
        }
    }
    return best                                  ? Mp4DemuxError::none
           : handler == fourcc('s', 'o', 'u', 'n') ? Mp4DemuxError::no_audio
                                                   : Mp4DemuxError::no_video;
}

Mp4DemuxError parse_description(Mp4Demuxer* demuxer, const Box& stsd) noexcept {
    Box entry{};
    if (!first_description(demuxer->encoded, stsd, &entry))
        return Mp4DemuxError::malformed;
    if (entry.type != fourcc('a', 'v', 'c', '1') && entry.type != fourcc('a', 'v', 'c', '3'))
        return Mp4DemuxError::unsupported;
    // ISO/IEC 14496-12 VisualSampleEntry is 78 bytes after its box header.
    if (entry.end - entry.payload < 78u)
        return Mp4DemuxError::malformed;
    const u8* visual = demuxer->encoded.data + entry.payload;
    demuxer->width = (static_cast<u32>(visual[24]) << 8u) | visual[25];
    demuxer->height = (static_cast<u32>(visual[26]) << 8u) | visual[27];
    Box avcc{};
    if (demuxer->width == 0 || demuxer->height == 0 ||
        !find_box(demuxer->encoded, entry.payload + 78u, entry.end, fourcc('a', 'v', 'c', 'C'), &avcc) ||
        avcc.end - avcc.payload < 7u || avcc.payload > ~0u || avcc.end - avcc.payload > ~0u)
        return Mp4DemuxError::malformed;
    const u8 length_bytes = static_cast<u8>((demuxer->encoded.data[avcc.payload + 4u] & 3u) + 1u);
    if (length_bytes == 3u)
        return Mp4DemuxError::malformed;
    demuxer->nal_length_bytes = length_bytes;
    demuxer->avcc_offset = static_cast<u32>(avcc.payload);
    demuxer->avcc_bytes = static_cast<u32>(avcc.end - avcc.payload);
    // Walk all extensions so a truncated or duplicate pasp cannot silently
    // turn an anamorphic stream into square pixels. Other valid boxes and
    // QuickTime's zero terminator are ignored.
    u64 extension_cursor = entry.payload + 78u;
    while (!atom_list_end(demuxer->encoded, extension_cursor, entry.end)) {
        Box extension{};
        if (!next_box(demuxer->encoded, &extension_cursor, entry.end, &extension))
            return Mp4DemuxError::malformed;
        if (extension.type != fourcc('p', 'a', 's', 'p'))
            continue;
        if (demuxer->sar_width || extension.end - extension.payload != 8u)
            return Mp4DemuxError::malformed;
        demuxer->sar_width = read_u32(demuxer->encoded.data + extension.payload);
        demuxer->sar_height = read_u32(demuxer->encoded.data + extension.payload + 4u);
        if (!demuxer->sar_width || !demuxer->sar_height)
            return Mp4DemuxError::malformed;
    }
    return Mp4DemuxError::none;
}

bool initialize_runs(Mp4Demuxer* demuxer) noexcept {
    if (demuxer->stts_count == 0 || demuxer->stsc_count == 0 ||
        demuxer->chunk_count == 0 || demuxer->sample_count == 0) {
        return false;
    }
    const u8* encoded = demuxer->encoded.data;
    demuxer->stts_left = read_u32(encoded + demuxer->stts_offset);
    demuxer->stts_delta = read_u32(encoded + demuxer->stts_offset + 4u);
    if (demuxer->stts_left == 0 || demuxer->stts_delta == 0) {
        return false;
    }
    if (demuxer->ctts_count != 0) {
        demuxer->ctts_left = read_u32(encoded + demuxer->ctts_offset);
        const u32 encoded_delta =
            read_u32(encoded + demuxer->ctts_offset + 4u);
        demuxer->ctts_delta = demuxer->ctts_version == 0
                                  ? static_cast<i64>(encoded_delta)
                                  : static_cast<i32>(encoded_delta);
        if (demuxer->ctts_left == 0) {
            return false;
        }
    }
    return read_u32(encoded + demuxer->stsc_offset) == 1u &&
           read_u32(encoded + demuxer->stsc_offset + 4u) != 0u;
}

bool load_chunk(Mp4Demuxer* demuxer) noexcept {
    if (demuxer->chunk_index >= demuxer->chunk_count) {
        return false;
    }
    const u8* encoded = demuxer->encoded.data;
    while (demuxer->stsc_index + 1u < demuxer->stsc_count) {
        const u32 next_first = read_u32(encoded + demuxer->stsc_offset +
                                        (demuxer->stsc_index + 1u) * 12u);
        if (next_first == 0 || next_first > demuxer->chunk_index + 1u) {
            break;
        }
        ++demuxer->stsc_index;
    }
    const u8* mapping = encoded + demuxer->stsc_offset +
                        demuxer->stsc_index * 12u;
    demuxer->samples_left_in_chunk = read_u32(mapping + 4u);
    if (demuxer->samples_left_in_chunk == 0) {
        return false;
    }
    const u8* chunk = encoded + demuxer->chunk_offset_table +
                      static_cast<u64>(demuxer->chunk_index) *
                          (demuxer->chunk_offsets_64 ? 8u : 4u);
    const u64 previous_end=demuxer->chunk_byte_offset;
    demuxer->chunk_byte_offset = demuxer->chunk_offsets_64
                                     ? read_u64(chunk)
                                     : read_u32(chunk);
    if (demuxer->chunk_index && demuxer->chunk_byte_offset < previous_end) return false;
    // Supported chunks are ordered, disjoint, and entirely inside mdat, so one
    // top-level cursor only moves forward.
    if (demuxer->chunk_byte_offset < demuxer->mdat_begin) return false;
    while (demuxer->chunk_byte_offset >= demuxer->mdat_end) {
        Box box{};
        if (!next_box(demuxer->encoded,&demuxer->mdat_cursor,demuxer->encoded.count,&box)) return false;
        if (box.type != fourcc('m','d','a','t')) continue;
        demuxer->mdat_begin=box.payload;demuxer->mdat_end=box.end;
        if (demuxer->chunk_byte_offset < box.payload) return false;
    }
    return demuxer->chunk_byte_offset >= demuxer->mdat_begin &&
           demuxer->chunk_byte_offset < demuxer->mdat_end;
}

bool consume_stts(Mp4Demuxer* demuxer, u32* delta) noexcept {
    if (demuxer->stts_left == 0) {
        if (++demuxer->stts_index >= demuxer->stts_count) {
            return false;
        }
        const u8* run = demuxer->encoded.data + demuxer->stts_offset +
                        demuxer->stts_index * 8u;
        demuxer->stts_left = read_u32(run);
        demuxer->stts_delta = read_u32(run + 4u);
        if (demuxer->stts_left == 0 || demuxer->stts_delta == 0) {
            return false;
        }
    }
    *delta = demuxer->stts_delta;
    --demuxer->stts_left;
    return true;
}

bool consume_ctts(Mp4Demuxer* demuxer, i64* delta) noexcept {
    if (demuxer->ctts_count == 0) {
        *delta = 0;
        return true;
    }
    if (demuxer->ctts_left == 0) {
        if (++demuxer->ctts_index >= demuxer->ctts_count) {
            return false;
        }
        const u8* run = demuxer->encoded.data + demuxer->ctts_offset +
                        demuxer->ctts_index * 8u;
        demuxer->ctts_left = read_u32(run);
        const u32 encoded_delta = read_u32(run + 4u);
        demuxer->ctts_delta = demuxer->ctts_version == 0
                                  ? static_cast<i64>(encoded_delta)
                                  : static_cast<i32>(encoded_delta);
        if (demuxer->ctts_left == 0) {
            return false;
        }
    }
    *delta = demuxer->ctts_delta;
    --demuxer->ctts_left;
    return true;
}

Mp4DemuxError edits(Mp4Demuxer& d,const Box& moov,const Box& track,
                    u64 edit_end_ticks) noexcept {
    Box edts{},elst{},mvhd{};
    if (!find_box(d.encoded,track.payload,track.end,fourcc('e','d','t','s'),&edts)) return Mp4DemuxError::none;
    d.has_edit_list=true;
    if (!find_box(d.encoded,edts.payload,edts.end,fourcc('e','l','s','t'),&elst) ||
        !find_box(d.encoded,moov.payload,moov.end,fourcc('m','v','h','d'),&mvhd) ||
        elst.end-elst.payload<8 || mvhd.end-mvhd.payload<20) return Mp4DemuxError::malformed;
    const u32 version=read_u32(d.encoded.data+elst.payload);
    if (version!=0 && version!=0x01000000u) return Mp4DemuxError::unsupported;
    const u32 movie_version=d.encoded[static_cast<u32>(mvhd.payload)];
    const u64 at=mvhd.payload+(movie_version==1?20u:12u);
    if (movie_version>1 || at+4>mvhd.end) return Mp4DemuxError::unsupported;
    const u32 movie_scale=read_u32(d.encoded.data+at);
    const u32 count=read_u32(d.encoded.data+elst.payload+4);
    const u32 stride=version?20u:12u;
    if (!movie_scale || !count || count>2 || elst.end-elst.payload!=8+u64{count}*stride) return Mp4DemuxError::unsupported;
    d.movie_timescale=movie_scale;
    for (u32 i=0;i<count;++i) {
        const auto* p=d.encoded.data+elst.payload+8+u64{i}*stride;
        const u64 span=version?read_u64(p):read_u32(p);
        const u64 media_bits=version?read_u64(p+8):read_u32(p+4);
        const i64 media=version?static_cast<i64>(media_bits):static_cast<i32>(media_bits);
        if (!span || read_u32(p+(version?16:8))!=0x00010000u) return Mp4DemuxError::unsupported;
        const u64 duration=scale_us(span,movie_scale);
        if (duration>0x7fffffffffffffffull) return Mp4DemuxError::unsupported;
        if (media==-1 && i==0 && count==2) { d.empty_edit_us=duration;d.empty_edit_ticks=span;continue; }
        if (media<0 || i+1!=count || static_cast<u64>(media)>=edit_end_ticks ||
            duration>0x7fffffffffffffffull-d.empty_edit_us) return Mp4DemuxError::unsupported;
        // Edits address the composition timeline, not the STTS decode extent.
        // B-frame muxing can put the presentation tail after the decode tail
        // and use a positive media_time to remove the initial CTS delay.
        const u64 available=scale_us(edit_end_ticks-static_cast<u64>(media),d.timescale);
        const u64 tolerance=(1'000'000ull+movie_scale-1)/movie_scale;
        if(duration>available && duration-available>tolerance)return Mp4DemuxError::unsupported;
        if(span>~u64{0}-d.empty_edit_ticks)return Mp4DemuxError::unsupported;
        d.media_start_ticks=media;d.duration_us=d.empty_edit_us+duration;d.movie_duration_ticks=d.empty_edit_ticks+span;
    }
    return Mp4DemuxError::none;
}

Mp4DemuxError initialize_tables(Mp4Demuxer* demuxer,ConstSlice<u8> encoded,
    u32 handler,Box* description,Mp4SourceAvailability availability) noexcept {
    if (!demuxer || !description || !byte_range_valid(encoded) || !encoded.data || encoded.count < 16u ||
        !byte_range_valid({reinterpret_cast<const u8*>(demuxer),sizeof(*demuxer)}) ||
        byte_ranges_overlap(encoded,{reinterpret_cast<const u8*>(demuxer),sizeof(*demuxer)})) {
        return Mp4DemuxError::invalid_input;
    }
    reset_value<Mp4Demuxer>(demuxer);
    if (encoded.count>(availability.available?kMediaMaximumProgressiveVideoBytes:kMp4DemuxMaximumEncodedBytes))
        return Mp4DemuxError::unsupported;
    demuxer->encoded = encoded;
    Box moov{};
    u64 top{};bool found_moov=false,found_mdat=false;
    while (top<encoded.count) {
        Box box{};if (!next_box(encoded,&top,encoded.count,&box)) return Mp4DemuxError::malformed;
        if (box.type==fourcc('m','o','o','f')) return Mp4DemuxError::unsupported;
        if (box.type==fourcc('m','d','a','t')) found_mdat=true;
        if (box.type==fourcc('m','o','o','v')) {if(found_moov)return Mp4DemuxError::malformed;moov=box;found_moov=true;}
    }
    if (!found_moov) return Mp4DemuxError::malformed;
    Box fragments{};
    if (find_box(encoded,moov.payload,moov.end,fourcc('m','v','e','x'),&fragments)) return Mp4DemuxError::unsupported;

    Box mdia{}, stbl{}, selected{};
    bool enabled{};
    const Mp4DemuxError chosen = select_track(encoded, moov, handler, &selected);
    if (chosen != Mp4DemuxError::none) return chosen;
    if (!media_track(encoded, selected, handler, &mdia, &stbl) ||
        !track_header(encoded, selected, &enabled, &demuxer->rotation))
        return Mp4DemuxError::malformed;
    if (!found_mdat) return Mp4DemuxError::malformed;
    Box minf{},dinf{},dref{};
    if (!find_box(encoded,mdia.payload,mdia.end,fourcc('m','i','n','f'),&minf)) return Mp4DemuxError::malformed;
    if (find_box(encoded,minf.payload,minf.end,fourcc('d','i','n','f'),&dinf)) {
        if (!find_box(encoded,dinf.payload,dinf.end,fourcc('d','r','e','f'),&dref) || dref.end-dref.payload<8 ||
            read_u32(encoded.data+dref.payload)!=0 || read_u32(encoded.data+dref.payload+4)!=1) return Mp4DemuxError::unsupported;
        u64 cursor=dref.payload+8;Box entry{};
        if (!next_box(encoded,&cursor,dref.end,&entry)) return Mp4DemuxError::unsupported;
        // ISO 'url ' and QuickTime 'alis' entries both flag self-contained media
        // with bit 0; any alias record after that flag is never followed.
        const bool self_reference = entry.type==fourcc('u','r','l',' ') ? entry.end-entry.payload==4 :
            entry.type==fourcc('a','l','i','s') && entry.end-entry.payload>=4;
        if (!self_reference || cursor!=dref.end || read_u32(encoded.data+entry.payload)!=1)
            return Mp4DemuxError::unsupported;
    }

    Box mdhd{};
    Box stsd{};
    Box stts{};
    Box ctts{};
    Box stsc{};
    Box stsz{};
    Box stco{};
    Box co64{};
    Box stss{};
    if (!find_box(encoded, mdia.payload, mdia.end,
                  fourcc('m', 'd', 'h', 'd'), &mdhd) ||
        mdhd.end - mdhd.payload < 20u ||
        !find_box(encoded, stbl.payload, stbl.end,
                  fourcc('s', 't', 's', 'd'), &stsd) ||
        !find_box(encoded, stbl.payload, stbl.end,
                  fourcc('s', 't', 't', 's'), &stts) ||
        !find_box(encoded, stbl.payload, stbl.end,
                  fourcc('s', 't', 's', 'c'), &stsc) ||
        !find_box(encoded, stbl.payload, stbl.end,
                  fourcc('s', 't', 's', 'z'), &stsz)) {
        return Mp4DemuxError::malformed;
    }
    const u8 mdhd_version = encoded.data[mdhd.payload];
    const u64 timescale_offset = mdhd.payload + (mdhd_version == 1 ? 20u : 12u);
    if ((mdhd_version != 0 && mdhd_version != 1) ||
        timescale_offset + 4u > mdhd.end) {
        return Mp4DemuxError::unsupported;
    }
    demuxer->timescale = read_u32(encoded.data + timescale_offset);
    if (demuxer->timescale == 0 ||
        !table(encoded, stts, 8u, &demuxer->stts_offset,
               &demuxer->stts_count) ||
        !table(encoded, stsc, 12u, &demuxer->stsc_offset,
               &demuxer->stsc_count)) {
        return Mp4DemuxError::malformed;
    }

    if (stsz.end - stsz.payload < 12u || read_u32(encoded.data+stsz.payload)!=0) {
        return Mp4DemuxError::malformed;
    }
    demuxer->constant_sample_bytes = read_u32(encoded.data + stsz.payload + 4u);
    demuxer->sample_count = read_u32(encoded.data + stsz.payload + 8u);
    demuxer->stsz_offset = static_cast<u32>(stsz.payload + 12u);
    if (demuxer->sample_count == 0 ||
        (demuxer->constant_sample_bytes == 0 &&
         static_cast<u64>(demuxer->sample_count) * 4u >
             stsz.end - (stsz.payload + 12u))) {
        return Mp4DemuxError::malformed;
    }

    if (find_box(encoded, stbl.payload, stbl.end,
                 fourcc('c', 't', 't', 's'), &ctts)) {
        if (!table(encoded, ctts, 8u, &demuxer->ctts_offset,
                   &demuxer->ctts_count)) {
            return Mp4DemuxError::malformed;
        }
        demuxer->ctts_version = encoded.data[ctts.payload];
        if (!demuxer->ctts_count || demuxer->ctts_version > 1u) {
            return Mp4DemuxError::unsupported;
        }
    }
    if (find_box(encoded, stbl.payload, stbl.end,
                 fourcc('s', 't', 's', 's'), &stss) &&
        !table(encoded, stss, 4u, &demuxer->stss_offset,
               &demuxer->stss_count)) {
        return Mp4DemuxError::malformed;
    }
    demuxer->sync_table_present=stss.end!=0;
    if (find_box(encoded, stbl.payload, stbl.end,
                 fourcc('s', 't', 'c', 'o'), &stco)) {
        if (!table(encoded, stco, 4u, &demuxer->chunk_offset_table,
                   &demuxer->chunk_count)) {
            return Mp4DemuxError::malformed;
        }
    } else if (find_box(encoded, stbl.payload, stbl.end,
                        fourcc('c', 'o', '6', '4'), &co64)) {
        if (!table(encoded, co64, 8u, &demuxer->chunk_offset_table,
                   &demuxer->chunk_count)) {
            return Mp4DemuxError::malformed;
        }
        demuxer->chunk_offsets_64 = true;
    } else {
        return Mp4DemuxError::malformed;
    }
    Box extra_offsets{};
    if (stco.end && find_box(encoded,stbl.payload,stbl.end,fourcc('c','o','6','4'),&extra_offsets))return Mp4DemuxError::malformed;
    // Validate complete run totals once, including unconsumed trailing runs.
    u64 total{},ticks{};
    for(u32 i=0;i<demuxer->stts_count;++i){const auto* p=encoded.data+demuxer->stts_offset+u64{i}*8;
        const u32 n=read_u32(p),delta=read_u32(p+4);
        if(!n||!delta)return Mp4DemuxError::malformed;total+=n;ticks+=u64{n}*delta;
        if(total>demuxer->sample_count || ticks>0x7fffffffffffffffull)return Mp4DemuxError::malformed;}
    if(total!=demuxer->sample_count)return Mp4DemuxError::malformed;
    demuxer->media_duration_ticks=ticks;demuxer->duration_us=scale_us(ticks,demuxer->timescale);
    demuxer->movie_timescale=demuxer->timescale;demuxer->movie_duration_ticks=ticks;
    if(demuxer->duration_us>0x7fffffffffffffffull)return Mp4DemuxError::unsupported;
    if(demuxer->ctts_count){total=0;for(u32 i=0;i<demuxer->ctts_count;++i){const u32 n=read_u32(encoded.data+demuxer->ctts_offset+u64{i}*8);
        if(!n)return Mp4DemuxError::malformed;total+=n;if(total>demuxer->sample_count)return Mp4DemuxError::malformed;}
        if(total!=demuxer->sample_count)return Mp4DemuxError::malformed;}
    total=0;
    for(u32 i=0;i<demuxer->stsc_count;++i){const auto* p=encoded.data+demuxer->stsc_offset+u64{i}*12;
        const u32 first=read_u32(p),n=read_u32(p+4),next=i+1<demuxer->stsc_count?read_u32(p+12):demuxer->chunk_count+1;
        if(!n || (i==0&&first!=1) || first>=next || next>u64{demuxer->chunk_count}+1 || read_u32(p+8)!=1)return Mp4DemuxError::unsupported;
        total+=u64{next-first}*n;if(total>demuxer->sample_count)return Mp4DemuxError::malformed;}
    if(total!=demuxer->sample_count)return Mp4DemuxError::malformed;
    u32 previous{};
    for(u32 i=0;i<demuxer->stss_count;++i){const u32 n=read_u32(encoded.data+demuxer->stss_offset+u64{i}*4);
        if(n<=previous || n>demuxer->sample_count)return Mp4DemuxError::malformed;previous=n;}
    demuxer->ready = true;
    mp4_demux_reset(demuxer);
    if (!demuxer->ready) {
        return Mp4DemuxError::malformed;
    }
    // Validate samples and derive the raw composition endpoint before edits.
    // media_duration_ticks stays the independent STTS decode total.
    u64 presentation_end_ticks{};
    Mp4Demuxer scan = *demuxer;
    for (u32 i = 0; i < demuxer->sample_count; ++i) {
        const auto sample = mp4_demux_next(&scan);
        if (!sample.ok()) {
            demuxer->ready = false;
            return sample.error;
        }
        if (sample.presentation_time_ticks > 0x7fffffffffffffffll - sample.duration_ticks) {
            demuxer->ready = false;
            return Mp4DemuxError::unsupported;
        }
        const i64 end = sample.presentation_time_ticks + sample.duration_ticks;
        if (end > 0)
            presentation_end_ticks = maximum(presentation_end_ticks, static_cast<u64>(end));
        if (sample.presentation_time_us_signed >= 0) {
            const u64 pts = static_cast<u64>(sample.presentation_time_us_signed);
            if (sample.duration_us > 0x7fffffffffffffffull - pts) {
                demuxer->ready = false;
                return Mp4DemuxError::unsupported;
            }
            demuxer->duration_us = maximum(demuxer->duration_us, pts + sample.duration_us);
        }
    }
    demuxer->movie_duration_ticks = maximum(demuxer->movie_duration_ticks, presentation_end_ticks);
    // Negative composition offsets can end before the STTS extent; keep that
    // extent so a declared trailing duration stays valid. Positive offsets may
    // extend it and must not drop valid final pictures.
    const u64 edit_end_ticks = maximum(demuxer->media_duration_ticks, presentation_end_ticks);
    const auto edited = edits(*demuxer, moov, selected, edit_end_ticks);
    if (edited != Mp4DemuxError::none) {
        demuxer->ready = false;
        return edited;
    }
    *description=stsd;
    demuxer->availability=availability;
    return Mp4DemuxError::none;
}

bool sample_key_less(i64 pts,u32 index,i64 bound_pts,u32 bound_index) noexcept {
    return pts<bound_pts||(pts==bound_pts&&index<bound_index);
}

Mp4DemuxError seek_sync(Mp4Demuxer* d,i64 target,Mp4SeekPoint* point,SyncClassifier classify) noexcept {
    Mp4Demuxer scan,selected{};copy_value(&scan,*d);mp4_demux_reset(&scan);
    const Mp4SourceAvailability availability=scan.availability;
    reset_value(&scan.availability);
    Mp4SeekPoint result{};bool found=false;
    if(!availability.available){
        while(scan.sample_index<scan.sample_count){Mp4Demuxer before;copy_value(&before,scan);const auto unit=mp4_demux_next(&scan);
            if(!unit.ok())return unit.error;
            if(!unit.sync||unit.presentation_time_us_signed>target)continue;
            const auto kind=classify(unit.bytes,d->nal_length_bytes);
            if(kind==SyncKind::malformed)return Mp4DemuxError::malformed;
            if(kind==SyncKind::unsupported)return Mp4DemuxError::unsupported;
            if(kind==SyncKind::random_access&&(!found||unit.presentation_time_us_signed>=result.presentation_time_us)){
                copy_value(&selected,before);result={unit.sample_index,unit.decode_time_us,unit.presentation_time_us_signed};found=true;}
        }
    }else{
        // Payloads may be absent: classify only the best remaining candidate,
        // ordered by (pts, sample index) exactly as the complete scan selects.
        i64 bound_pts=0x7fffffffffffffffll;u32 bound_index=~0u;
        for(u32 attempt=0;attempt<64&&!found;++attempt){
            Mp4Demuxer cursor,best_before{};copy_value(&cursor,scan);Mp4AccessUnit best{};bool any=false;
            while(cursor.sample_index<cursor.sample_count){Mp4Demuxer before;copy_value(&before,cursor);const auto unit=mp4_demux_next(&cursor);
                if(!unit.ok())return unit.error;
                if(!unit.sync||unit.presentation_time_us_signed>target||
                    !sample_key_less(unit.presentation_time_us_signed,unit.sample_index,bound_pts,bound_index))continue;
                if(!any||!sample_key_less(unit.presentation_time_us_signed,unit.sample_index,
                                          best.presentation_time_us_signed,best.sample_index)){
                    best=unit;copy_value(&best_before,before);any=true;}
            }
            if(!any)return Mp4DemuxError::unsupported;
            const u64 offset=static_cast<u64>(best.bytes.data-d->encoded.data);
            if(!availability.available(availability.context,offset,best.bytes.count))return Mp4DemuxError::data_pending;
            const auto kind=classify(best.bytes,d->nal_length_bytes);
            if(kind==SyncKind::malformed)return Mp4DemuxError::malformed;
            if(kind==SyncKind::unsupported)return Mp4DemuxError::unsupported;
            if(kind==SyncKind::random_access){
                copy_value(&selected,best_before);result={best.sample_index,best.decode_time_us,best.presentation_time_us_signed};found=true;}
            bound_pts=best.presentation_time_us_signed;bound_index=best.sample_index;
        }
    }
    if(!found)return Mp4DemuxError::unsupported;
    selected.availability=availability;
    copy_value(d,selected);*point=result;return Mp4DemuxError::none;
}

} // namespace mp4_demux_detail
using namespace mp4_demux_detail;

Mp4DemuxError mp4_demux_initialize(Mp4Demuxer* demuxer,ConstSlice<u8> encoded,
                                   Mp4SourceAvailability availability) noexcept {
    if(!demuxer)return Mp4DemuxError::invalid_input;
    Box description{};
    const auto result=initialize_tables(demuxer,encoded,fourcc('v','i','d','e'),&description,availability);
    // invalid_input rejected the arguments before touching demuxer, which may alias them.
    if(result!=Mp4DemuxError::none){if(result!=Mp4DemuxError::invalid_input)demuxer->ready=false;return result;}
    const auto described=parse_description(demuxer,description);
    if(described!=Mp4DemuxError::none)demuxer->ready=false;
    return described;
}

void mp4_demux_reset(Mp4Demuxer* demuxer) noexcept {
    if (!demuxer || !demuxer->ready) {
        return;
    }
    demuxer->sample_index = 0;
    demuxer->decode_time = 0;
    demuxer->chunk_byte_offset = 0;
    demuxer->chunk_index = 0;
    demuxer->samples_left_in_chunk = 0;
    demuxer->stsc_index = 0;
    demuxer->stts_index = 0;
    demuxer->ctts_index = 0;
    demuxer->stss_index = 0;
    demuxer->mdat_cursor=demuxer->mdat_begin=demuxer->mdat_end=0;
    if (!initialize_runs(demuxer)) {
        demuxer->ready = false;
    }
}

Mp4AccessUnit mp4_demux_next(Mp4Demuxer* demuxer) noexcept {
    Mp4AccessUnit result{};
    if (!demuxer || !demuxer->ready) {
        result.error = Mp4DemuxError::invalid_input;
        return result;
    }
    if (demuxer->sample_index >= demuxer->sample_count) {
        result.error = Mp4DemuxError::end_of_stream;
        return result;
    }
    if (demuxer->samples_left_in_chunk == 0 && !load_chunk(demuxer)) {
        result.error = Mp4DemuxError::malformed;
        return result;
    }
    const u32 sample_bytes = demuxer->constant_sample_bytes != 0
                                 ? demuxer->constant_sample_bytes
                                 : read_u32(demuxer->encoded.data +
                                            demuxer->stsz_offset +
                                            demuxer->sample_index * 4u);
    if (sample_bytes == 0 || demuxer->chunk_byte_offset > demuxer->encoded.count ||
        sample_bytes > demuxer->encoded.count - demuxer->chunk_byte_offset ||
        demuxer->chunk_byte_offset>demuxer->mdat_end || sample_bytes>demuxer->mdat_end-demuxer->chunk_byte_offset) {
        result.error = Mp4DemuxError::malformed;
        return result;
    }
    u32 duration = 0;
    i64 composition_delta = 0;
    if (!consume_stts(demuxer, &duration) ||
        !consume_ctts(demuxer, &composition_delta)) {
        result.error = Mp4DemuxError::malformed;
        return result;
    }
    if (composition_delta>0 && demuxer->decode_time>0x7fffffffffffffffull-static_cast<u64>(composition_delta)) {
        result.error=Mp4DemuxError::unsupported;return result;
    }
    const i64 presentation = static_cast<i64>(demuxer->decode_time) +
                             composition_delta;
    if(presentation<(-0x7fffffffffffffffll-1)+demuxer->media_start_ticks){result.error=Mp4DemuxError::unsupported;return result;}
    const i64 relative=presentation-demuxer->media_start_ticks;
    const u64 absolute=relative<0?static_cast<u64>(-(relative+1))+1:static_cast<u64>(relative);
    const u64 scaled=scale_us(absolute,demuxer->timescale);
    if(scaled>0x7fffffffffffffffull || (relative>=0&&scaled>0x7fffffffffffffffull-demuxer->empty_edit_us)){
        result.error=Mp4DemuxError::unsupported;return result;
    }
    const i64 mapped=(relative<0?-static_cast<i64>(scaled):static_cast<i64>(scaled))+static_cast<i64>(demuxer->empty_edit_us);
    bool sync = !demuxer->sync_table_present;
    if (!sync && demuxer->stss_index < demuxer->stss_count) {
        const u32 sync_sample = read_u32(
            demuxer->encoded.data + demuxer->stss_offset +
            demuxer->stss_index * 4u);
        sync = sync_sample == demuxer->sample_index + 1u;
        if (sync) {
            ++demuxer->stss_index;
        } else if (sync_sample < demuxer->sample_index + 1u) {
            result.error = Mp4DemuxError::malformed;
            return result;
        }
    }
    result.bytes = ConstSlice<u8>{
        demuxer->encoded.data + demuxer->chunk_byte_offset, sample_bytes};
    result.decode_time_us = scale_us(demuxer->decode_time, demuxer->timescale);
    result.presentation_time_us_signed=mapped;
    result.presentation_time_us=mapped<0?0:static_cast<u64>(mapped);
    result.presentation_time_ticks=presentation;
    result.duration_us = scale_us(duration, demuxer->timescale);
    result.duration_ticks=duration;
    result.sample_index = demuxer->sample_index;
    result.sync = sync;
    result.error = Mp4DemuxError::none;

    demuxer->decode_time += duration;
    demuxer->chunk_byte_offset += sample_bytes;
    --demuxer->samples_left_in_chunk;
    ++demuxer->sample_index;
    if (demuxer->samples_left_in_chunk == 0) {
        ++demuxer->chunk_index;
    }
    return result;
}

ConstSlice<u8> mp4_demux_avcc(const Mp4Demuxer& demuxer) noexcept {
    if (!demuxer.ready || demuxer.avcc_offset > demuxer.encoded.count ||
        demuxer.avcc_bytes > demuxer.encoded.count - demuxer.avcc_offset) {
        return {};
    }
    return ConstSlice<u8>{demuxer.encoded.data + demuxer.avcc_offset,
                          demuxer.avcc_bytes};
}

Mp4DemuxError mp4_demux_seek_sample(Mp4Demuxer* d,u32 sample) noexcept {
    if(!d||!d->ready||sample>=d->sample_count)return Mp4DemuxError::invalid_input;
    Mp4Demuxer cursor=*d;mp4_demux_reset(&cursor);
    for(u32 i=0;i<sample;++i){const auto unit=mp4_demux_next(&cursor);if(!unit.ok())return unit.error;}
    *d=cursor;return Mp4DemuxError::none;
}
Mp4DemuxError mp4_demux_seek_sync(Mp4Demuxer* d,i64 target,Mp4SeekPoint* point) noexcept {
    if(!d||!point||!d->ready||target<0||static_cast<u64>(target)>d->duration_us ||
        (d->nal_length_bytes!=1&&d->nal_length_bytes!=2&&d->nal_length_bytes!=4))return Mp4DemuxError::invalid_input;
    const ConstSlice<u8> output{reinterpret_cast<const u8*>(point),sizeof(*point)};
    if(!byte_range_valid(output)||byte_ranges_overlap(output,{reinterpret_cast<const u8*>(d),sizeof(*d)})||
        byte_ranges_overlap(output,d->encoded))return Mp4DemuxError::invalid_input;
    return seek_sync(d,target,point,[](ConstSlice<u8> bytes,u8 length_bytes) noexcept {
        u32 at{};bool idr=false,non_idr=false;
        while(at<bytes.count){if(bytes.count-at<length_bytes)return SyncKind::malformed;
            u32 n{};for(u32 i=0;i<length_bytes;++i)n=(n<<8)|bytes[at++];
            if(!n||n>bytes.count-at)return SyncKind::malformed;
            const u8 type=bytes[at]&31;
            if(bytes[at]&128)return SyncKind::malformed;
            if(type==5)idr=true;if(type>=1&&type<=4)non_idr=true;at+=n;}
        return idr&&!non_idr?SyncKind::random_access:SyncKind::other;
    });
}
bool mp4_demux_available(const Mp4Demuxer& d,ConstSlice<u8> bytes) noexcept {
    if(!d.availability.available)return true;
    if(!bytes.data||bytes.data<d.encoded.data||static_cast<u64>(bytes.data-d.encoded.data)>d.encoded.count)return false;
    return d.availability.available(d.availability.context,static_cast<u64>(bytes.data-d.encoded.data),bytes.count);
}
Mp4AccessUnit mp4_demux_next_ready(Mp4Demuxer* d) noexcept {
    if(!d)return mp4_demux_next(d);
    Mp4Demuxer cursor;copy_value(&cursor,*d);
    auto unit=mp4_demux_next(&cursor);
    if(unit.ok()&&!mp4_demux_available(*d,unit.bytes)){unit.error=Mp4DemuxError::data_pending;return unit;}
    copy_value(d,cursor);
    return unit;
}

} // namespace maicord
