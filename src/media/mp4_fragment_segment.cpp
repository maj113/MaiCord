#include "maicord/media/mp4_fragment.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "mp4_demux_detail.hpp"
// Container parsing, once per segment and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using namespace mp4_demux_detail;
using E = Mp4DemuxError;
E next_fragment(ConstSlice<u8> bytes, const Mp4FragmentTrack& track, u32 offset,
                Mp4Fragment* output, u32* next) noexcept {
    u64 at = offset; Box moof{}, mdat{};
    for (u32 skipped = 0; skipped <= 64; ++skipped) {
        if (at == bytes.count) return E::end_of_stream;
        if (!next_box(bytes, &at, bytes.count, &moof)) return E::malformed;
        if (moof.type == fourcc('m','o','o','f')) break;
        if (moof.type != fourcc('e','m','s','g') || skipped == 64) return E::unsupported;
    }
    if (!next_box(bytes, &at, bytes.count, &mdat) || mdat.type != fourcc('m','d','a','t')) return E::malformed;
    const u32 extent = static_cast<u32>(mdat.end - moof.begin);
    Mp4Fragment value{};
    const auto error = mp4_fragment_initialize({bytes.data + moof.begin, extent}, moof.begin, extent, track.defaults, &value);
    if (error != E::none) return error;
    *output = value; *next = static_cast<u32>(mdat.end); return E::none;
}
bool input_valid(ConstSlice<u8> bytes, const Mp4FragmentTrack& track, const void* output, u32 size) noexcept {
    return bytes.data && bytes.count && bytes.count <= 8 * 1024 * 1024 && byte_range_valid(bytes) &&
        output && byte_range_valid(output, size) && track.defaults.track_id && track.timescale &&
        !byte_ranges_overlap(bytes.data, bytes.count, output, size) &&
        !byte_ranges_overlap(&track, sizeof(track), output, size) &&
        !byte_ranges_overlap(track.configuration.data, track.configuration.count, output, size);
}
}
Mp4DemuxError mp4_media_segment_fragment(ConstSlice<u8> bytes, const Mp4FragmentTrack& track,
    u32 offset, Mp4Fragment* output, u32* next) noexcept {
    if (!input_valid(bytes, track, output, sizeof(*output)) || !input_valid(bytes, track, next, sizeof(*next)) ||
        byte_ranges_overlap(output, sizeof(*output), next, sizeof(*next)) || offset > bytes.count) return E::invalid_input;
    return next_fragment(bytes, track, offset, output, next);
}
Mp4DemuxError mp4_media_segment_initialize(ConstSlice<u8> bytes, const Mp4FragmentTrack& track,
                                            Mp4MediaSegment* output) noexcept {
    if (!input_valid(bytes, track, output, sizeof(*output))) return E::invalid_input;
    u64 at = 0; Box child{};
    if (!next_box(bytes, &at, bytes.count, &child)) return E::malformed;
    if (child.type == fourcc('s','t','y','p')) {
        if (child.end - child.payload < 8 || (child.end - child.payload) % 4) return E::malformed;
        if (!next_box(bytes, &at, bytes.count, &child)) return E::malformed;
    }
    Mp4MediaSegment value{};
    const bool indexed = child.type == fourcc('s','i','d','x');
    if (indexed) {
        Mp4FragmentIndex index{};
        const auto error = mp4_fragment_index_initialize(
            {bytes.data + child.begin, static_cast<u32>(child.end - child.begin)}, child.begin, bytes.count, &index);
        if (error != E::none) return error;
        if (index.count != 1) return E::unsupported;
        if (index.track_id != track.defaults.track_id || index.timescale != track.timescale ||
            !mp4_fragment_index_range(index, 0, &value.range) || value.range.offset != at ||
            value.range.bytes != bytes.count - at) return E::malformed;
    } else at = child.begin;
    u64 end = 0; u32 cursor = static_cast<u32>(at), fragments = 0;
    while (cursor < bytes.count) {
        Mp4Fragment fragment{}; u32 next{};
        const auto error = next_fragment(bytes, track, cursor, &fragment, &next);
        if (error == E::end_of_stream) { cursor = bytes.count; break; }
        if (error != E::none) return error;
        if (++fragments > 1024 || fragment.count > 65536 - value.samples) return E::unsupported;
        if (value.samples && fragment.first_time != end) return E::malformed;
        if (!value.samples) {
            value.fragment = fragment; value.next_fragment = next;
            if (!indexed) {
                value.range.offset = cursor; value.range.bytes = bytes.count - cursor;
                value.range.time = fragment.first_time;
            }
        }
        end = fragment.first_time + fragment.duration;
        value.samples += fragment.count;
        if (!track.audio) {
            Mp4FragmentSample sample{};
            while (mp4_fragment_next(&fragment, &sample) == E::none) {
                if (sample.presentation_time > 0x7fffffffffffffffll - sample.duration) return E::malformed;
                const i64 finish = sample.presentation_time + sample.duration;
                if (finish > 0) value.presentation_end = maximum(value.presentation_end, static_cast<u64>(finish));
            }
            if (fragment.index != fragment.count) return E::malformed;
        }
        cursor = next;
    }
    if (!value.samples) return E::malformed;
    if (!indexed) value.range.duration = end - value.range.time;
    if (track.audio && (value.fragment.first_time != value.range.time || end - value.fragment.first_time != value.range.duration))
        return E::malformed;
    copy_value(output, value);
    return E::none;
}
} // namespace maicord
#pragma clang attribute pop
