#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
// Segment bookkeeping, once per segment: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using E = Mp4DemuxError;
void advance(u64& serial) noexcept { if (!++serial) ++serial; }
}
bool mp4_segment_store_range_fits(const Mp4SegmentStore& s, u32 number, const Mp4FragmentRange& range) noexcept {
    for (const auto& prior : s.slots) if (prior.bytes && prior.number != number) {
        const auto& a = prior.number < number ? prior.media.range : range;
        const auto& b = prior.number < number ? range : prior.media.range;
        if (a.time + a.duration > b.time || (s.live &&
            (prior.number + 1 == number || number + 1 == prior.number) && a.time + a.duration != b.time)) return false;
    }
    return true;
}
bool mp4_segment_store_overlaps(const Mp4SegmentStore& s, const void* p, u32 bytes) noexcept {
    if (!byte_range_valid(p, bytes) || byte_ranges_overlap(&s, sizeof(s), p, bytes) ||
        byte_ranges_overlap(s.initialization.base, s.initialization.reserved, p, bytes)) return true;
    for (const auto& slot : s.slots)
        if (byte_ranges_overlap(slot.storage.base, slot.storage.reserved, p, bytes)) return true;
    return false;
}
Mp4DemuxError mp4_segment_store_initialize(Mp4SegmentStore* s, ConstSlice<u8> init, bool audio,
    u32 count, u64 duration, u32 timescale, bool live) noexcept {
    if (!s || !byte_range_valid(s, sizeof(*s)) || s->initialization.base || s->count ||
        s->slots[0].storage.base || s->slots[1].storage.base || !init.data || !init.count || init.count > 1024 * 1024 ||
        !byte_range_valid(init) || mp4_segment_store_overlaps(*s, init.data, init.count) || !count || count > 8192 ||
        !timescale || timescale > 1000000000 || !duration || duration > u64{86400} * timescale) return E::invalid_input;
    Mp4SegmentStore value{};
    const auto parsed = mp4_fragment_track_initialize(init, audio, &value.track);
    if (parsed != E::none) return parsed;
    if (!reserved_bytes_initialize(&value.initialization, init.count) ||
        !reserved_bytes_commit(&value.initialization, init.count)) {
        reserved_bytes_release(&value.initialization); return E::out_of_memory;
    }
    copy_bytes(value.initialization.base, init.data, init.count);
    value.track.configuration.data = value.initialization.base + (value.track.configuration.data - init.data);
    value.initialization_bytes = init.count;
    value.count = count; value.nominal_duration = duration; value.nominal_timescale = timescale;
    value.live = live;
    value.generation = s->generation; value.request_serial = s->request_serial;
    advance(value.generation); advance(value.request_serial);
    copy_value(s, value);
    return E::none;
}
const Mp4SegmentSlot* mp4_segment_store_find(const Mp4SegmentStore& s, u32 number) noexcept {
    for (const auto& slot : s.slots) if (slot.bytes && slot.number == number) return &slot;
    return nullptr;
}
Mp4DemuxError mp4_segment_store_request(Mp4SegmentStore* s, u32 number) noexcept {
    if (!s || !s->initialization.base) return E::invalid_input;
    if (number >= s->count) {
        if (number == s->count && s->live && !s->ended) { s->refresh_pending = true; return E::data_pending; }
        return E::invalid_input;
    }
    if (mp4_segment_store_find(*s, number)) return E::none;
    if (number < s->first_available) return E::unsupported;
    if (!s->pending || s->demand != number) {
        advance(s->request_serial); s->demand = number; s->pending = true;
    }
    return E::data_pending;
}
Mp4DemuxError mp4_segment_store_accept(Mp4SegmentStore* s, u32 number, u64 serial, ConstSlice<u8> bytes) noexcept {
    if (!s || s->transport || !s->initialization.base || !s->pending || s->demand != number || serial != s->request_serial ||
        number >= s->count || !bytes.data || !bytes.count || !byte_range_valid(bytes) ||
        mp4_segment_store_overlaps(*s, bytes.data, bytes.count)) return E::invalid_input;
    const u32 capacity = s->track.audio ? 1024 * 1024 : 8 * 1024 * 1024;
    if (bytes.count > capacity) return E::unsupported;
    Mp4MediaSegment parsed{};
    const auto error = mp4_media_segment_initialize(bytes, s->track, &parsed);
    if (error != E::none) return error;
    // Each ordinal describes a disjoint interval. This also rejects a server
    // repeating the old file for a different requested segment.
    if (!mp4_segment_store_range_fits(*s, number, parsed.range)) return E::malformed;
    const u32 chosen = s->active == 0 ? 1 : 0;
    auto& slot = s->slots[chosen];
    if ((!slot.storage.base && !reserved_bytes_initialize(&slot.storage, capacity)) ||
        !reserved_bytes_commit(&slot.storage, bytes.count)) return E::out_of_memory;
    copy_bytes(slot.storage.base, bytes.data, bytes.count);
    parsed.fragment.metadata.data = slot.storage.base + (parsed.fragment.metadata.data - bytes.data);
    parsed.range.index = number;
    slot.media = parsed; slot.number = number; slot.bytes = bytes.count;
    s->available_end = maximum(s->available_end, parsed.range.time + parsed.range.duration);
    s->available_presentation_end = maximum(s->available_presentation_end, parsed.presentation_end);
    s->received_count = maximum(s->received_count, number + 1);
    s->pending = false; advance(s->generation);
    return E::none;
}
bool mp4_segment_store_retain(Mp4SegmentStore* s, u32 number) noexcept {
    if (!s) return false;
    for (u8 i = 0; i < 2; ++i) if (s->slots[i].bytes && s->slots[i].number == number) {
        if (s->active != i) { s->active = i; advance(s->generation); }
        return true;
    }
    return false;
}
void mp4_segment_store_cancel(Mp4SegmentStore* s) noexcept {
    if (!s) return;
    s->seek_active = false;
    s->refresh_pending = false;
    if (!s->pending) return;
    s->pending = false; advance(s->request_serial);
}
Mp4DemuxError mp4_segment_store_extend(Mp4SegmentStore* s, u32 first, u32 count, bool ended) noexcept {
    if (!s || !s->initialization.base || !s->live || !count || first >= count) return E::invalid_input;
    if (first < s->first_available || count < s->count || first > s->count ||
        (s->ended && (!ended || first != s->first_available || count != s->count))) return E::malformed;
    if (first != s->first_available || count != s->count || ended != s->ended) advance(s->generation);
    s->first_available = first; s->count = count; s->ended = ended; s->refresh_pending = false;
    return E::none;
}
void mp4_segment_store_release(Mp4SegmentStore* s) noexcept {
    if (!s) return;
    reserved_bytes_release(&s->initialization);
    for (auto& slot : s->slots) reserved_bytes_release(&slot.storage);
    u64 generation = s->generation, serial = s->request_serial;
    advance(generation); advance(serial);
    reset_value(s); s->generation = generation; s->request_serial = serial;
}
} // namespace maicord
#pragma clang attribute pop
