#include "maicord/media/mp4_fragment_source.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_range.hpp"
#include "mp4_demux_detail.hpp"
// Container parsing, once per sample and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using namespace mp4_demux_detail;
using E = Mp4DemuxError;
constexpr u32 metadata_limit = 1024 * 1024;
bool available(ProgressiveSource& s, ProgressiveTrack track, u64 first, u64 bytes) noexcept {
    const auto reader = progressive_source_reader(&s, track);
    return reader.available(reader.context, first, bytes);
}
E header(ProgressiveSource& s, ProgressiveTrack track, u64 at, u64 end, Box* output) noexcept {
    if (at > end || end > s.total_bytes || end - at < 8) return E::malformed;
    if (!available(s, track, at, 8)) return E::data_pending;
    u64 size = read_u32(s.base + at), count = 8;
    if (size == 1) {
        if (end - at < 16) return E::malformed;
        if (!available(s, track, at, 16)) return E::data_pending;
        size = read_u64(s.base + at + 8); count = 16;
    } else if (!size) size = end - at;
    if (size < count || size > end - at) return E::malformed;
    *output = {read_u32(s.base + at + 4), at, at + count, at + size};
    return E::none;
}
E read_fragment(const Mp4FragmentSource& stream, const Mp4FragmentRange& range, Mp4Fragment* output) noexcept {
    auto& source = *stream.source;
    const u64 first = range.offset, end = first + range.bytes;
    Box moof{}, mdat{};
    auto error = header(source, stream.reader, first, end, &moof);
    if (error != E::none) return error;
    if (moof.type != fourcc('m','o','o','f')) return E::unsupported;
    if (moof.end - first > 256 * 1024) return E::unsupported;
    error = header(source, stream.reader, moof.end, end, &mdat);
    if (error != E::none) return error;
    if (mdat.type != fourcc('m','d','a','t') || mdat.end != end) return E::malformed;
    const u32 bytes = static_cast<u32>(mdat.payload - first);
    if (!available(source, stream.reader, first, bytes)) return E::data_pending;
    Mp4Fragment fragment{};
    error = mp4_fragment_initialize({source.base + first, bytes}, first, range.bytes,
                                    stream.track.defaults, &fragment);
    if (error != E::none) return error;
    copy_value(output, fragment);
    return E::none;
}
E load(Mp4FragmentSource& stream) noexcept {
    Mp4Fragment fragment{};
    if (stream.segments) {
        auto& store = *stream.segments;
        const auto error = mp4_segment_store_request(&store, stream.segment.index);
        if (error != E::none) return error;
        const auto* slot = mp4_segment_store_find(store, stream.segment.index);
        copy_value(&fragment, slot->media.fragment);
        if (stream.fragment.count && (fragment.first_time < stream.fragment.first_time + stream.fragment.duration ||
            (store.live && fragment.first_time != stream.fragment.first_time + stream.fragment.duration)))
            return E::malformed;
        if (!mp4_segment_store_retain(&store, stream.segment.index)) return E::data_pending;
        stream.segment = slot->media.range; copy_value(&stream.fragment, fragment); stream.loaded = true;
        stream.next_fragment = slot->media.next_fragment;
        return E::none;
    }
    const auto error = read_fragment(stream, stream.segment, &fragment);
    if (error != E::none) return error;
    if (stream.fragment.count && fragment.first_time < stream.fragment.first_time + stream.fragment.duration)
        return E::malformed;
    if (!progressive_source_retain_metadata(stream.source, stream.reader, stream.segment.offset, fragment.metadata.count)) return E::data_pending;
    // sidx holds presentation timing; tfdt holds decode timing. They need not
    // be equal when pictures reorder. Every packet retains its real timestamps.
    copy_value(&stream.fragment, fragment);
    stream.loaded = true;
    return E::none;
}
} // namespace
bool mp4_fragment_source_valid(const Mp4FragmentSource& stream) noexcept {
    return (stream.source != nullptr) != (stream.segments != nullptr);
}
bool mp4_fragment_source_overlaps(const Mp4FragmentSource& stream, const void* p, u32 bytes) noexcept {
    if (!byte_range_valid(p, bytes) || byte_ranges_overlap(&stream, sizeof(stream), p, bytes)) return true;
    if (stream.segments) return mp4_segment_store_overlaps(*stream.segments, p, bytes);
    return stream.source && (byte_ranges_overlap(stream.source, sizeof(*stream.source), p, bytes) ||
        byte_ranges_overlap(stream.source->base, stream.source->total_bytes, p, bytes));
}
ConstSlice<u8> mp4_fragment_source_bytes(const Mp4FragmentSource& stream) noexcept {
    return stream.segments ? ConstSlice<u8>{stream.segments->initialization.base, stream.segments->initialization_bytes}
        : stream.source ? progressive_source_bytes(*stream.source) : ConstSlice<u8>{};
}
namespace {
u64 generation(const Mp4FragmentSource& stream) noexcept {
    return stream.segments ? stream.segments->generation : stream.source->generation;
}
E sample_bytes(Mp4FragmentSource& stream, u32 segment, const Mp4FragmentSample& sample, ConstSlice<u8>* output) noexcept {
    if (stream.segments) {
        const auto* slot = mp4_segment_store_find(*stream.segments, segment);
        if (!slot) return E::data_pending;
        if (sample.offset > slot->bytes || sample.bytes > slot->bytes - sample.offset) return E::malformed;
        *output = {slot->storage.base + sample.offset, sample.bytes};
    } else {
        if (!available(*stream.source, stream.reader, sample.offset, sample.bytes)) return E::data_pending;
        *output = {stream.source->base + sample.offset, sample.bytes};
    }
    return E::none;
}
}
Mp4DemuxError mp4_fragment_source_initialize_segments(Mp4SegmentStore* store, Mp4FragmentSource* output) noexcept {
    if (!store || !store->count || !output || output->source || output->segments ||
        mp4_segment_store_overlaps(*store, output, sizeof(*output))) return E::invalid_input;
    auto error = mp4_segment_store_request(store, 0);
    if (error != E::none) return error;
    const auto* first = mp4_segment_store_find(*store, 0);
    if (!mp4_segment_store_retain(store, 0)) return E::data_pending;
    error = mp4_segment_store_request(store, store->live ? 0 : store->count - 1);
    if (error != E::none) return error;
    const auto* last = mp4_segment_store_find(*store, store->live ? 0 : store->count - 1);
    const u64 end = last->media.range.time + last->media.range.duration;
    if (end <= first->media.range.time) return E::malformed;
    if (end - first->media.range.time > u64{86400} * store->track.timescale) return E::unsupported;
    Mp4FragmentSource value{};
    value.segments = store; value.track = store->track;
    value.index = {{}, 0, first->media.range.time, end - first->media.range.time,
                   store->track.defaults.track_id, store->track.timescale, store->count};
    value.segment = first->media.range; value.fragment = first->media.fragment; value.loaded = true;
    value.next_fragment = first->media.next_fragment;
    value.reader = value.track.audio ? ProgressiveTrack::audio : ProgressiveTrack::video;
    if (!value.track.audio) {
        value.presentation_end = last->media.presentation_end;
        if (!value.presentation_end) return E::malformed;
    }
    copy_value(output, value);
    return E::none;
}
bool mp4_fragment_source_refresh(Mp4FragmentSource* stream) noexcept {
    if (!stream || !stream->segments || !stream->segments->live || !stream->index.count) return false;
    const auto& store = *stream->segments;
    if (store.count < stream->index.count || store.available_end <= stream->index.first_time ||
        store.available_end - stream->index.first_time < stream->index.duration) return false;
    stream->index.count = store.count;
    stream->index.duration = store.available_end - stream->index.first_time;
    if (!stream->track.audio) stream->presentation_end = maximum(stream->presentation_end, store.available_presentation_end);
    return true;
}
Mp4DemuxError mp4_fragment_source_initialize(ProgressiveSource* source, bool audio,
                                             Mp4FragmentSource* output) noexcept {
    if (!source || !source->base || !source->total_bytes || !output || output->source || output->segments ||
        byte_ranges_overlap(progressive_source_bytes(*source), {reinterpret_cast<const u8*>(output), sizeof(*output)}))
        return E::invalid_input;
    const auto reader = audio ? ProgressiveTrack::audio : ProgressiveTrack::video;
    if (source->readers[audio ? 1 : 0].metadata_bytes) return E::invalid_input;
    Box moov{}, ftyp{}, sidx{};
    u64 at = 0;
    for (u32 boxes = 0; boxes < 64 && !(moov.end && ftyp.end && sidx.end); ++boxes) {
        Box child{};
        const auto error = header(*source, reader, at, source->total_bytes, &child);
        if (error != E::none) return error;
        if (child.end > metadata_limit) return E::unsupported;
        if (child.type == fourcc('m','o','o','f') || child.type == fourcc('m','d','a','t')) return E::unsupported;
        Box* target = child.type == fourcc('m','o','o','v') ? &moov : child.type == fourcc('f','t','y','p') ? &ftyp
                     : child.type == fourcc('s','i','d','x') ? &sidx : nullptr;
        if (target) { if (target->end) return E::malformed; *target = child; }
        at = child.end;
    }
    if (!moov.end || !ftyp.end || !sidx.end || ftyp.begin > moov.begin) return E::malformed;
    if (!available(*source, reader, 0, at)) return E::data_pending;
    Mp4FragmentSource value{}; value.source = source; value.reader = reader;
    auto error = mp4_fragment_track_initialize({source->base, static_cast<u32>(maximum(moov.end, ftyp.end))}, audio, &value.track);
    if (error != E::none) return error;
    error = mp4_fragment_index_initialize({source->base + sidx.begin, static_cast<u32>(sidx.end - sidx.begin)},
                                          sidx.begin, source->total_bytes, &value.index);
    if (error != E::none) return error;
    if (value.track.defaults.track_id != value.index.track_id || value.track.timescale != value.index.timescale ||
        !mp4_fragment_index_range(value.index, 0, &value.segment) || value.segment.offset < at) return E::malformed;
    progressive_source_pin(source, 0, at);
    copy_value(output, value);
    return E::none;
}
Mp4DemuxError mp4_fragment_source_find(Mp4FragmentSource* stream, u64 time, Mp4FragmentRange* output) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || !output ||
        mp4_fragment_source_overlaps(*stream, output, sizeof(*output))) return E::invalid_input;
    if (stream->segments && stream->segments->live && !mp4_fragment_source_refresh(stream)) return E::malformed;
    if (time < stream->index.first_time || time - stream->index.first_time >= stream->index.duration) return E::invalid_input;
    if (!stream->segments) return mp4_fragment_index_seek(stream->index, time, output) ? E::none : E::malformed;
    auto& store = *stream->segments;
    stream->preparing = true;
    const u64 relative = time - stream->index.first_time, scale = stream->track.timescale;
    const u64 seconds = relative / scale;
    if (seconds > 86400) return E::unsupported;
    // Exact resident intervals take precedence over a nominal playlist hint.
    // A hint can otherwise evict the very file we already have, or request an
    // expired live ordinal. The current cursor/lease stays pinned until adopt.
    for (const auto& slot : store.slots) if (slot.bytes) {
        const auto& range = slot.media.range;
        if (time >= range.time && time - range.time < range.duration) { *output = range; return E::none; }
    }
    if (!store.seek_active || store.seek_time != time) {
        // Divide before multiplying: both timescales are bounded and only the
        // remainder product is needed. This hint never becomes a media clock.
        const u64 nominal = seconds * store.nominal_timescale +
            (relative % scale) * store.nominal_timescale / scale;
        store.seek_number = static_cast<u32>(minimum<u64>(nominal / store.nominal_duration, store.count - 1));
        if (store.locate && (!store.locate(store.seek_context, seconds * 1000000 +
            (relative % scale) * 1000000 / scale, &store.seek_number) || store.seek_number >= store.count)) return E::malformed;
        store.seek_attempts = 0; store.seek_direction = 0; store.seek_time = time; store.seek_active = true;
    }
    while (store.seek_attempts < 64) {
        const auto error = mp4_segment_store_request(&store, store.seek_number);
        if (error != E::none) return error;
        const auto* slot = mp4_segment_store_find(store, store.seek_number);
        const auto& range = slot->media.range;
        if (time >= range.time && time - range.time < range.duration) { *output = range; return E::none; }
        const i32 direction = time < range.time ? -1 : 1;
        if ((store.seek_direction && store.seek_direction != direction) ||
            (direction < 0 ? !store.seek_number : store.seek_number == store.count - 1)) return E::malformed;
        store.seek_direction = direction; ++store.seek_attempts;
        if (direction < 0) --store.seek_number; else ++store.seek_number;
    }
    return E::unsupported;
}
Mp4DemuxError mp4_fragment_source_peek(Mp4FragmentSource* stream, Mp4FragmentPacket* output) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || !output ||
        mp4_fragment_source_overlaps(*stream, output, sizeof(*output))) return E::invalid_input;
    if (stream->segments && stream->segments->live && !mp4_fragment_source_refresh(stream)) return E::malformed;
    mp4_fragment_source_cancel_prepare(stream);
    if (stream->ended) return E::end_of_stream;
    if (stream->segments && stream->loaded && stream->fragment.index == stream->fragment.count) {
        const auto* slot = mp4_segment_store_find(*stream->segments, stream->segment.index);
        if (!slot) return E::malformed;
        Mp4Fragment next{}; u32 offset{};
        const auto error = mp4_media_segment_fragment({slot->storage.base, slot->bytes}, stream->track,
            stream->next_fragment, &next, &offset);
        if (error == E::none) {
            copy_value(&stream->fragment, next); stream->next_fragment = offset;
        } else if (error != E::end_of_stream) return error;
    }
    if (stream->loaded && stream->fragment.index == stream->fragment.count) {
        Mp4FragmentRange next{};
        const bool following = stream->segments ? stream->segment.index + 1 < stream->index.count
            : mp4_fragment_index_follow(stream->index, stream->segment, &next);
        if (!following) {
            if (stream->segment.index + 1 < stream->index.count) return E::malformed;
            if (stream->segments && stream->segments->live && !stream->segments->ended)
                return mp4_segment_store_request(stream->segments, stream->index.count);
            stream->ended = true;
            if (stream->source) stream->source->readers[stream->reader == ProgressiveTrack::audio ? 1 : 0] = {stream->source};
            return E::end_of_stream;
        }
        if (stream->segments) next.index = stream->segment.index + 1;
        stream->segment = next; stream->loaded = false;
        if (stream->source) progressive_source_retain_metadata(stream->source, stream->reader, 0, 0);
    }
    if (!stream->loaded) {
        const auto error = load(*stream);
        if (error != E::none) return error;
    }
    Mp4Fragment cursor;
    copy_value(&cursor, stream->fragment);
    Mp4FragmentPacket value{}; value.segment = stream->segment.index;
    const auto error = mp4_fragment_next(&cursor, &value.sample);
    if (error != E::none) return error;
    if (value.sample.bytes > kProgressiveMinimumAheadBytes) return E::unsupported;
    const auto resident = sample_bytes(*stream, value.segment, value.sample, &value.bytes);
    if (resident != E::none) return resident;
    stream->pending = true;
    *output = value;
    return E::none;
}
bool mp4_fragment_source_commit(Mp4FragmentSource* stream) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || !stream->pending) return false;
    Mp4FragmentSample sample{};
    if (mp4_fragment_next(&stream->fragment, &sample) != E::none) return false;
    stream->pending = false;
    return true;
}
bool mp4_fragment_source_time(const Mp4FragmentSource& stream, i64 ticks, i64* output) noexcept {
    if (!mp4_fragment_source_valid(stream) || !output || !stream.track.timescale || !stream.track.movie_timescale) return false;
    const auto& track = stream.track;
    constexpr i64 limit = 0x7fffffffffffffffll;
    if (ticks < (-limit - 1) + track.media_start) return false;
    const i64 relative = ticks - track.media_start;
    const u64 absolute = relative < 0 ? static_cast<u64>(-(relative + 1)) + 1 : static_cast<u64>(relative);
    const u64 scaled = scale_us(absolute, track.timescale), empty = scale_us(track.empty_edit, track.movie_timescale);
    if (scaled > limit || empty > limit || (relative >= 0 && scaled > limit - empty)) return false;
    const i64 mapped = (relative < 0 ? -static_cast<i64>(scaled) : static_cast<i64>(scaled)) + static_cast<i64>(empty);
    *output = mapped;
    return true;
}
namespace {
E packet_unit(const Mp4FragmentSource& stream, const Mp4FragmentPacket& packet, Mp4AccessUnit* output) noexcept {
    const auto& track = stream.track;
    const auto& sample = packet.sample;
    i64 mapped{};
    if (!mp4_fragment_source_time(stream, sample.presentation_time, &mapped)) return E::unsupported;
    constexpr u64 limit = 0x7fffffffffffffffull;
    Mp4AccessUnit unit{};
    unit.bytes = packet.bytes; unit.sample_index = sample.index; unit.sync = sample.sync;
    unit.decode_time_us = scale_us(sample.decode_time, track.timescale);
    unit.presentation_time_us_signed = mapped; unit.presentation_time_us = mapped < 0 ? 0 : static_cast<u64>(mapped);
    unit.presentation_time_ticks = sample.presentation_time;
    unit.duration_us = scale_us(sample.duration, track.timescale); unit.duration_ticks = sample.duration;
    if (unit.decode_time_us > limit || !unit.duration_us || unit.duration_us > limit) return E::unsupported;
    *output = unit;
    return E::none;
}
} // namespace
Mp4DemuxError mp4_fragment_source_prepare(Mp4FragmentSource* stream, u32 segment, u32 sample,
                                          Mp4FragmentPosition* output) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || !output ||
        mp4_fragment_source_overlaps(*stream, output, sizeof(*output))) return E::invalid_input;
    if (stream->segments && stream->segments->live && !mp4_fragment_source_refresh(stream)) return E::malformed;
    Mp4FragmentPosition value{};
    value.owner = stream;
    if (stream->segments) {
        if (segment >= stream->index.count) return E::invalid_input;
        stream->preparing = true;
        const auto error = mp4_segment_store_request(stream->segments, segment);
        if (error != E::none) return error;
        const auto* slot = mp4_segment_store_find(*stream->segments, segment);
        value.segment = slot->media.range; copy_value(&value.fragment, slot->media.fragment);
        value.next_fragment = slot->media.next_fragment;
        if (sample >= slot->media.samples) return E::invalid_input;
        while (sample >= value.fragment.count) {
            sample -= value.fragment.count;
            Mp4Fragment following{}; u32 next{};
            const auto parsed = mp4_media_segment_fragment({slot->storage.base, slot->bytes}, stream->track,
                value.next_fragment, &following, &next);
            if (parsed != E::none) return parsed;
            copy_value(&value.fragment, following); value.next_fragment = next;
        }
    } else {
        mp4_fragment_source_cancel_prepare(stream);
        if (!mp4_fragment_index_range(stream->index, segment, &value.segment)) return E::invalid_input;
        const auto error = read_fragment(*stream, value.segment, &value.fragment);
        if (error != E::none) return error;
    }
    if (sample >= value.fragment.count) return E::invalid_input;
    for (u32 i = 0; i < sample; ++i) {
        Mp4FragmentSample skipped{};
        if (mp4_fragment_next(&value.fragment, &skipped) != E::none) return E::malformed;
    }
    Mp4Fragment next;
    copy_value(&next, value.fragment);
    if (mp4_fragment_next(&next, &value.packet.sample) != E::none) return E::malformed;
    const auto& selected = value.packet.sample;
    if (selected.bytes > kProgressiveMinimumAheadBytes) return E::unsupported;
    if (stream->source && !progressive_source_retain_metadata(stream->source, stream->reader, value.segment.offset,
                                            value.fragment.metadata.count, true)) return E::data_pending;
    const auto resident = sample_bytes(*stream, segment, selected, &value.packet.bytes);
    if (resident != E::none) return resident;
    value.packet.segment = segment;
    value.generation = generation(*stream);
    copy_value(output, value);
    return E::none;
}
void mp4_fragment_source_cancel_prepare(Mp4FragmentSource* stream) noexcept {
    if (stream && stream->source) progressive_source_retain_metadata(stream->source, stream->reader, 0, 0, true);
    if (stream && stream->segments && stream->preparing) {
        mp4_segment_store_cancel(stream->segments); stream->preparing = false;
    }
}
bool mp4_fragment_source_adopt(Mp4FragmentSource* stream, const Mp4FragmentPosition& position) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || position.owner != stream ||
        position.generation != generation(*stream)) return false;
    if (stream->segments) {
        if (!mp4_segment_store_retain(stream->segments, position.segment.index)) return false;
    } else if (!progressive_source_available(*stream->source, position.packet.sample.offset, position.packet.sample.bytes) ||
               !progressive_source_retain_metadata(stream->source, stream->reader, position.segment.offset,
                                                   position.fragment.metadata.count)) return false;
    stream->segment = position.segment; copy_value(&stream->fragment, position.fragment);
    stream->next_fragment = position.next_fragment;
    stream->loaded = stream->pending = true; stream->ended = false;
    mp4_fragment_source_cancel_prepare(stream);
    if (stream->source) available(*stream->source, stream->reader, position.packet.sample.offset, position.packet.sample.bytes);
    return true;
}
Mp4DemuxError mp4_fragment_position_unit(const Mp4FragmentPosition& position, Mp4AccessUnit* output) noexcept {
    if (!position.owner || !mp4_fragment_source_valid(*position.owner) || !output ||
        position.generation != generation(*position.owner) ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&position), sizeof(position)},
                            {reinterpret_cast<const u8*>(output), sizeof(*output)}) ||
        mp4_fragment_source_overlaps(*position.owner, output, sizeof(*output))) return E::invalid_input;
    return packet_unit(*position.owner, position.packet, output);
}
Mp4DemuxError mp4_fragment_source_unit(Mp4FragmentSource* stream, Mp4AccessUnit* output) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream) || !output ||
        mp4_fragment_source_overlaps(*stream, output, sizeof(*output))) return E::invalid_input;
    Mp4FragmentPacket packet{};
    const auto error = mp4_fragment_source_peek(stream, &packet);
    if (error != E::none) return error;
    return packet_unit(*stream, packet, output);
}

bool mp4_fragment_source_duration(const Mp4FragmentSource& stream, u64* output) noexcept {
    if (!mp4_fragment_source_valid(stream) || !output || !stream.track.timescale || !stream.track.movie_timescale) return false;
    const auto& track = stream.track;
    constexpr u64 limit = 0x7fffffffffffffffull;
    u64 duration{};
    if (track.edited && track.edit_duration)
        duration = scale_us(track.empty_edit + track.edit_duration, track.movie_timescale);
    else {
        const u64 end = stream.segments && stream.segments->live
            ? (track.audio ? stream.segments->available_end : stream.segments->available_presentation_end)
            : stream.presentation_end ? stream.presentation_end : stream.index.first_time + stream.index.duration;
        const u64 remaining = end > static_cast<u64>(track.media_start) ? end - track.media_start : 0;
        const u64 empty = scale_us(track.empty_edit, track.movie_timescale), media = scale_us(remaining, track.timescale);
        if (empty > limit || media > limit - empty) return false;
        duration = empty + media;
    }
    if (!duration || duration > limit) return false;
    *output = duration;
    return true;
}
bool mp4_fragment_source_select(Mp4FragmentSource* stream, u32 segment) noexcept {
    if (!stream || !mp4_fragment_source_valid(*stream)) return false;
    if (stream->segments && stream->segments->live && !mp4_fragment_source_refresh(stream)) return false;
    if (segment > stream->index.count) return false;
    Mp4FragmentRange selected{};
    if (stream->segments) selected.index = segment;
    else if (segment < stream->index.count && !mp4_fragment_index_range(stream->index, segment, &selected)) return false;
    mp4_fragment_source_cancel_prepare(stream);
    if (stream->source) progressive_source_retain_metadata(stream->source, stream->reader, 0, 0);
    stream->segment = selected; reset_value(&stream->fragment); stream->next_fragment = 0;
    stream->loaded = stream->pending = false;
    stream->ended = segment == stream->index.count && !(stream->segments && stream->segments->live && !stream->segments->ended);
    if (stream->segments) {
        mp4_segment_store_cancel(stream->segments);
        if (!stream->ended) mp4_segment_store_request(stream->segments, segment);
    } else if (!stream->ended) available(*stream->source, stream->reader, selected.offset, 8);
    else stream->source->readers[stream->reader == ProgressiveTrack::audio ? 1 : 0] = {stream->source};
    return true;
}
void mp4_fragment_source_release(Mp4FragmentSource* stream) noexcept {
    if (!stream) return;
    if (stream->source)
        stream->source->readers[stream->reader == ProgressiveTrack::audio ? 1 : 0] = {stream->source};
    if (stream->segments) { mp4_segment_store_cancel(stream->segments); stream->segments->active = 2; }
    reset_value(stream);
}
} // namespace maicord
#pragma clang attribute pop
