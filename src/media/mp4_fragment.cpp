#include "maicord/media/mp4_fragment.hpp"
#include "maicord/core/byte_range.hpp"
#include "mp4_demux_detail.hpp"

namespace maicord {
namespace {
using namespace mp4_demux_detail;
using E = Mp4DemuxError;
constexpr u64 limit = 0x7fffffffffffffffull;
template <typename T> bool output_valid(ConstSlice<u8> input, T* output) noexcept {
    const ConstSlice<u8> region{reinterpret_cast<const u8*>(output), sizeof(T)};
    return input.data && byte_range_valid(input) && output && byte_range_valid(region) &&
           !byte_ranges_overlap(input, region);
}
bool box(ConstSlice<u8> input, u32 type, Box* out) noexcept {
    u64 cursor = 0;
    return next_box(input, &cursor, input.count, out) && cursor == input.count && out->type == type;
}
bool add(u64 a, u64 b, u64* out) noexcept {
    if (b > ~u64{0} - a) return false;
    *out = a + b;
    return true;
}
bool offset(u64 base, i32 delta, u64* out) noexcept {
    if (delta >= 0) return add(base, static_cast<u32>(delta), out);
    const u64 magnitude = static_cast<u64>(-static_cast<i64>(delta));
    if (magnitude > base) return false;
    *out = base - magnitude;
    return true;
}
bool index_range(const Mp4FragmentIndex& index, u32 number, u64 time, bool seek,
                 Mp4FragmentRange* output) noexcept {
    if (!output_valid(index.entries, output) || !index.count || index.count > 8192 ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&index), sizeof(index)},
                            {reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        index.entries.count != index.count * 12 || (!seek && number >= index.count)) return false;
    u64 at = index.first_offset, ticks = index.first_time;
    for (u32 i = 0; i < index.count; ++i) {
        const auto* p = index.entries.data + i * 12;
        const u32 bytes = read_u32(p), duration = read_u32(p + 4), sap = read_u32(p + 8);
        if ((!seek && i == number) || (seek && time >= ticks && time - ticks < duration)) {
            *output = {at, ticks, duration, bytes, i, sap & 0x0fffffffu,
                       static_cast<u8>((sap >> 28) & 7), (sap >> 31) != 0};
            return true;
        }
        at += bytes;
        ticks += duration;
    }
    return false;
}
u32 stride(u32 flags) noexcept {
    return 4 * (((flags >> 8) & 1) + ((flags >> 9) & 1) + ((flags >> 10) & 1) + ((flags >> 11) & 1));
}
} // namespace
Mp4DemuxError mp4_fragment_defaults(ConstSlice<u8> bytes, Mp4FragmentDefaults* output) noexcept {
    if (!output_valid(bytes, output)) return E::invalid_input;
    Box trex{};
    if (!box(bytes, fourcc('t','r','e','x'), &trex) || trex.end - trex.payload != 24) return E::malformed;
    const auto* p = bytes.data + trex.payload;
    if (read_u32(p) || read_u32(p + 8) != 1) return E::unsupported;
    Mp4FragmentDefaults value{read_u32(p + 4), read_u32(p + 12), read_u32(p + 16), read_u32(p + 20)};
    if (!value.track_id || (value.flags & 0xf0000000u)) return E::malformed;
    *output = value;
    return E::none;
}
Mp4DemuxError mp4_fragment_index_initialize(ConstSlice<u8> bytes, u64 offset, u64 entity_bytes,
                                           Mp4FragmentIndex* output) noexcept {
    if (!output_valid(bytes, output) || offset > entity_bytes || bytes.count > entity_bytes - offset)
        return E::invalid_input;
    Box sidx{};
    if (!box(bytes, fourcc('s','i','d','x'), &sidx) || sidx.end - sidx.payload < 24) return E::malformed;
    const auto* p = bytes.data + sidx.payload;
    const u32 version = read_u32(p);
    if (version != 0 && version != 0x01000000) return E::unsupported;
    const u32 header = version ? 32 : 24;
    if (sidx.end - sidx.payload < header) return E::malformed;
    Mp4FragmentIndex value{};
    value.track_id = read_u32(p + 4); value.timescale = read_u32(p + 8);
    value.first_time = version ? read_u64(p + 12) : read_u32(p + 12);
    const u64 first = version ? read_u64(p + 20) : read_u32(p + 16);
    const u32 reserved_count = read_u32(p + header - 4);
    value.count = reserved_count & 65535;
    if (!value.track_id || !value.timescale || (reserved_count >> 16) || !value.count || value.count > 8192 ||
        sidx.end - sidx.payload - header != u64{value.count} * 12 || value.first_time > limit ||
        !add(offset + bytes.count, first, &value.first_offset) || value.first_offset > entity_bytes)
        return E::malformed;
    value.entries = {p + header, value.count * 12};
    u64 end = value.first_offset, time = value.first_time;
    for (u32 i = 0; i < value.count; ++i) {
        const auto* row = value.entries.data + i * 12;
        const u32 size = read_u32(row), duration = read_u32(row + 4), sap = read_u32(row + 8);
        if (size >> 31) return E::unsupported; // Nested sidx, not media bytes.
        if (size < 16 || !duration || size > entity_bytes - end || duration > limit - time ||
            ((sap >> 28) & 7) == 7 || (sap & 0x0fffffffu) >= duration) return E::malformed;
        end += size; time += duration;
    }
    value.duration = time - value.first_time;
    *output = value;
    return E::none;
}
bool mp4_fragment_index_range(const Mp4FragmentIndex& index, u32 number, Mp4FragmentRange* output) noexcept {
    return index_range(index, number, 0, false, output);
}
bool mp4_fragment_index_follow(const Mp4FragmentIndex& index, const Mp4FragmentRange& prior,
                                Mp4FragmentRange* output) noexcept {
    if (!output_valid(index.entries, output) || !index.count || index.count > 8192 ||
        index.entries.count != index.count * 12 || prior.index >= index.count - 1 ||
        prior.bytes > ~u64{0} - prior.offset || prior.time > limit || prior.duration > limit - prior.time ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&index), sizeof(index)},
                            {reinterpret_cast<const u8*>(output), sizeof(*output)})) return false;
    const u32 next = prior.index + 1;
    const auto* p = index.entries.data + next * 12;
    const u32 sap = read_u32(p + 8);
    *output = {prior.offset + prior.bytes, prior.time + prior.duration, read_u32(p + 4), read_u32(p), next,
               sap & 0x0fffffffu, static_cast<u8>((sap >> 28) & 7), (sap >> 31) != 0};
    return true;
}
bool mp4_fragment_index_seek(const Mp4FragmentIndex& index, u64 time, Mp4FragmentRange* output) noexcept {
    return index_range(index, 0, time, true, output);
}
void mp4_fragment_reset(Mp4Fragment* cursor) noexcept {
    if (!cursor) return;
    cursor->index = cursor->run_index = cursor->run_sample = 0;
    cursor->decode_time = cursor->first_time;
    cursor->media_cursor = cursor->run_count ? cursor->runs[0].offset : 0;
}
Mp4DemuxError mp4_fragment_next(Mp4Fragment* cursor, Mp4FragmentSample* output) noexcept {
    if (!cursor || !output_valid(cursor->metadata, output) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(cursor), sizeof(*cursor)},
                            {reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        !cursor->count || !cursor->run_count || cursor->run_count > 8) return E::invalid_input;
    if (cursor->index == cursor->count) return E::end_of_stream;
    if (cursor->run_index >= cursor->run_count) return E::malformed;
    auto& run = cursor->runs[cursor->run_index];
    const u32 width = stride(run.flags);
    const u64 at = u64{run.table} + u64{cursor->run_sample} * width;
    if (cursor->run_sample >= run.count || at > cursor->metadata.count || width > cursor->metadata.count - at)
        return E::malformed;
    const auto* p = cursor->metadata.data + at;
    Mp4FragmentSample value{cursor->media_cursor, cursor->decode_time, 0,
                            cursor->defaults.bytes, cursor->defaults.duration, cursor->index};
    u32 flags = (run.flags & 4) && !cursor->run_sample ? run.first_flags : cursor->defaults.flags;
    if (run.flags & 0x100) { value.duration = read_u32(p); p += 4; }
    if (run.flags & 0x200) { value.bytes = read_u32(p); p += 4; }
    if (run.flags & 0x400) { flags = read_u32(p); p += 4; }
    const i64 composition = !(run.flags & 0x800) ? 0 : (run.flags >> 24)
        ? static_cast<i32>(read_u32(p)) : static_cast<i64>(read_u32(p));
    if (!value.duration || !value.bytes || value.decode_time > limit ||
        value.duration > limit - value.decode_time || (flags & 0xf0000000u) ||
        (composition > 0 && static_cast<u64>(composition) > limit - value.decode_time) ||
        value.bytes > ~u64{0} - value.offset) return E::malformed;
    value.presentation_time = static_cast<i64>(value.decode_time) + composition;
    value.sync = !(flags & 0x10000) && ((flags >> 24) & 3) != 1;
    cursor->decode_time += value.duration;
    cursor->media_cursor += value.bytes;
    ++cursor->index;
    if (++cursor->run_sample == run.count) {
        ++cursor->run_index; cursor->run_sample = 0;
        if (cursor->run_index < cursor->run_count) cursor->media_cursor = cursor->runs[cursor->run_index].offset;
    }
    *output = value;
    return E::none;
}
Mp4DemuxError mp4_fragment_initialize(ConstSlice<u8> prefix, u64 source_offset, u64 segment_bytes,
                                     const Mp4FragmentDefaults& defaults, Mp4Fragment* output) noexcept {
    if (!output_valid(prefix, output) || !defaults.track_id || prefix.count < 16 ||
        prefix.count > segment_bytes || segment_bytes > ~u64{0} - source_offset) return E::invalid_input;
    Box moof{}; u64 cursor = 0;
    if (!next_box(prefix, &cursor, prefix.count, &moof) || moof.type != fourcc('m','o','o','f') ||
        moof.end > 256 * 1024 || prefix.count - moof.end < 8) return E::malformed;
    const auto* mdat = prefix.data + moof.end;
    u64 mdat_size = read_u32(mdat), mdat_header = 8;
    if (read_u32(mdat + 4) != fourcc('m','d','a','t')) return E::unsupported;
    if (mdat_size == 1) {
        if (prefix.count - moof.end < 16) return E::malformed;
        mdat_size = read_u64(mdat + 8); mdat_header = 16;
    } else if (!mdat_size) mdat_size = segment_bytes - moof.end;
    if (mdat_size <= mdat_header || mdat_size != segment_bytes - moof.end) return E::malformed;
    const u64 media_begin = source_offset + moof.end + mdat_header, media_end = source_offset + segment_bytes;
    Box traf{}, mfhd{};
    u32 track_ids[32]{}, tracks = 0; bool first_track = false;
    cursor = moof.payload;
    while (cursor < moof.end) {
        Box child{};
        if (!next_box(prefix, &cursor, moof.end, &child)) return E::malformed;
        if (child.type == fourcc('m','f','h','d')) {
            if (mfhd.end || child.end - child.payload != 8 || read_u32(prefix.data + child.payload)) return E::malformed;
            mfhd = child;
        } else if (child.type == fourcc('t','r','a','f')) {
            if (tracks == 32) return E::unsupported;
            Box header{}; u64 at = child.payload;
            while (at < child.end) {
                Box field{};
                if (!next_box(prefix, &at, child.end, &field)) return E::malformed;
                if (field.type == fourcc('t','f','h','d')) {
                    if (header.end || field.end - field.payload < 8) return E::malformed;
                    header = field;
                }
            }
            if (!header.end) return E::malformed;
            const u32 id = read_u32(prefix.data + header.payload + 4);
            if (!id) return E::malformed;
            for (u32 i = 0; i < tracks; ++i) if (track_ids[i] == id) return E::malformed;
            if (id == defaults.track_id) { traf = child; first_track = !tracks; }
            track_ids[tracks++] = id;
        }
    }
    if (!traf.end || !mfhd.end) return E::malformed;
    Mp4Fragment value{};
    value.metadata = {prefix.data, static_cast<u32>(moof.end)}; value.defaults = defaults;
    Box tfhd{}, tfdt{};
    bool roll_descriptions = false, roll_mapping = false;
    u32 ungrouped_samples = 0;
    cursor = traf.payload;
    while (cursor < traf.end) {
        Box child{};
        if (!next_box(prefix, &cursor, traf.end, &child)) return E::malformed;
        if (child.type == fourcc('t','f','h','d')) { if (tfhd.end) return E::malformed; tfhd = child; }
        else if (child.type == fourcc('t','f','d','t')) { if (tfdt.end) return E::malformed; tfdt = child; }
        else if (child.type == fourcc('t','r','u','n')) {
            if (value.run_count == 8 || child.end - child.payload < 8) return E::unsupported;
            auto& run = value.runs[value.run_count++];
            run.table = static_cast<u32>(child.payload); run.count = static_cast<u32>(child.end - child.payload);
        } else if (child.type == fourcc('s','g','p','d') || child.type == fourcc('s','b','g','p')) {
            const auto* group = prefix.data + child.payload;
            const u64 bytes = child.end - child.payload;
            if (bytes < 12) return E::malformed;
            if (read_u32(group + 4) != fourcc('r','o','l','l')) return E::unsupported;
            // Index zero means no group. Empty roll metadata changes neither
            // sync flags nor seek preroll; active recovery groups need support
            // beyond the native sample flags and remain rejected.
            if (child.type == fourcc('s','g','p','d')) {
                if (roll_descriptions) return E::malformed;
                roll_descriptions = true;
                if (bytes != 16 || read_u32(group) != 0x01000000 ||
                    read_u32(group + 8) != 2 || read_u32(group + 12)) return E::unsupported;
            } else {
                if (roll_mapping) return E::malformed;
                roll_mapping = true;
                if (read_u32(group)) return E::unsupported;
                const u32 entries = read_u32(group + 8);
                if (entries > 65536 || bytes != 12 + u64{entries} * 8) return E::malformed;
                for (u32 i = 0; i < entries; ++i) {
                    const u32 samples = read_u32(group + 12 + i * 8);
                    if (!samples || samples > 65536 - ungrouped_samples) return E::malformed;
                    if (read_u32(group + 16 + i * 8)) return E::unsupported;
                    ungrouped_samples += samples;
                }
            }
        } else if (child.type == fourcc('s','e','n','c') || child.type == fourcc('s','a','i','z') ||
                   child.type == fourcc('s','a','i','o') || child.type == fourcc('u','u','i','d') ||
                   child.type == fourcc('s','i','n','f')) return E::unsupported;
    }
    if (!tfhd.end || tfhd.end - tfhd.payload < 8 || !tfdt.end || !value.run_count) return E::malformed;
    const auto* p = prefix.data + tfhd.payload;
    const u32 flags = read_u32(p);
    if (flags & ~0x02003bu) return E::unsupported;
    if ((flags & 0x20001) == 0x20001 || read_u32(p + 4) != defaults.track_id) return E::malformed;
    // A later traf without an explicit/moof-relative base inherits the end
    // of another track's data. Never guess that base from the selected track.
    if (!first_track && !(flags & 0x20001)) return E::unsupported;
    const u32 header = 8 + ((flags & 1) ? 8 : 0) + ((flags & 2) ? 4 : 0) +
        ((flags & 8) ? 4 : 0) + ((flags & 16) ? 4 : 0) + ((flags & 32) ? 4 : 0);
    if (tfhd.end - tfhd.payload != header) return E::malformed;
    p += 8;
    u64 base = source_offset;
    if (flags & 1) { base = read_u64(p); p += 8; }
    if (flags & 2) { if (read_u32(p) != 1) return E::unsupported; p += 4; }
    if (flags & 8) { value.defaults.duration = read_u32(p); p += 4; }
    if (flags & 16) { value.defaults.bytes = read_u32(p); p += 4; }
    if (flags & 32) value.defaults.flags = read_u32(p);
    p = prefix.data + tfdt.payload;
    if (tfdt.end - tfdt.payload < 8) return E::malformed;
    const u32 version = read_u32(p);
    if (version != 0 && version != 0x01000000) return E::unsupported;
    if (tfdt.end - tfdt.payload != (version ? 12 : 8)) return E::malformed;
    value.first_time = version ? read_u64(p + 4) : read_u32(p + 4);
    u64 previous_end = media_begin;
    for (u32 i = 0; i < value.run_count; ++i) {
        auto& run = value.runs[i];
        const u32 run_bytes = run.count;
        p = prefix.data + run.table;
        run.flags = read_u32(p); run.count = read_u32(p + 4);
        const u32 width = stride(run.flags), head = 8 + ((run.flags & 1) ? 4 : 0) + ((run.flags & 4) ? 4 : 0);
        if (run.flags & ~0x01000f05u) return E::unsupported;
        if (!run.count || run.count > 65536 - value.count || ((run.flags & 0x404) == 0x404) ||
            u64{head} + u64{width} * run.count != run_bytes || (!i && !(run.flags & 1))) return E::malformed;
        run.offset = previous_end; p += 8;
        if (run.flags & 1) { if (!offset(base, static_cast<i32>(read_u32(p)), &run.offset)) return E::malformed; p += 4; }
        if (run.flags & 4) run.first_flags = read_u32(p);
        run.table += head;
        if (run.offset < previous_end || run.offset >= media_end) return E::malformed;
        value.count += run.count;
        // Validate all sizes/timestamps once without retaining a sample array.
        value.run_index = i; value.run_sample = 0; value.media_cursor = run.offset;
        if (!i) value.decode_time = value.first_time;
        for (u32 n = 0; n < run.count; ++n) {
            Mp4FragmentSample sample{};
            if (mp4_fragment_next(&value, &sample) != E::none || sample.offset > media_end ||
                sample.bytes > media_end - sample.offset) return E::malformed;
            previous_end = sample.offset + sample.bytes;
        }
    }
    if (ungrouped_samples > value.count) return E::malformed;
    value.duration = value.decode_time - value.first_time;
    mp4_fragment_reset(&value);
    *output = value;
    return E::none;
}
} // namespace maicord
