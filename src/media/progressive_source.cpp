#include "maicord/media/progressive_source.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
constexpr u32 kBlock = kProgressiveBlockBytes;

bool bit(const u64* words, u32 index) noexcept {
    return (words[index >> 6] >> (index & 63u)) & 1u;
}
void set_bit(u64* words, u32 index, bool value) noexcept {
    const u64 mask = u64{1} << (index & 63u);
    words[index >> 6] = value ? words[index >> 6] | mask : words[index >> 6] & ~mask;
}
u64 block_end(const ProgressiveSource& s, u32 block) noexcept {
    return minimum<u64>(u64{block + 1} * kBlock, s.total_bytes);
}
bool range_valid(const ProgressiveSource& s, u64 offset, u64 bytes) noexcept {
    return s.base && offset <= s.total_bytes && bytes <= s.total_bytes - offset;
}
// First absent block in [first, last); last when none.
u32 first_missing(const ProgressiveSource& s, u32 first, u32 last) noexcept {
    for (u32 b = first; b < last; ++b)
        if (!bit(s.present, b))
            return b;
    return last;
}
// Contiguous absent blocks from the first gap at/after offset, before limit.
ProgressiveRange gap_from(const ProgressiveSource& s, u64 offset, u64 limit, u32 maximum_bytes) noexcept {
    if (offset >= s.total_bytes)
        return {};
    const u32 last = static_cast<u32>((minimum<u64>(limit, s.total_bytes) + kBlock - 1) / kBlock);
    const u32 start = first_missing(s, static_cast<u32>(offset / kBlock), last);
    if (start >= last)
        return {};
    u32 stop = start + 1;
    const u32 count = maximum(1u, maximum_bytes / kBlock);
    while (stop < last && stop - start < count && !bit(s.present, stop))
        ++stop;
    const u64 first = u64{start} * kBlock;
    return {first, static_cast<u32>(block_end(s, stop - 1) - first)};
}
// A refusal is stale once its missing block has arrived; the reader then
// counts as a cursor for read-ahead until it queries again.
bool refused(const ProgressiveSource& s, const ProgressiveReader& r) noexcept {
    return r.pending && (r.missing >= s.total_bytes || !bit(s.present, static_cast<u32>(r.missing / kBlock)));
}
bool intersects(ProgressiveRange r, u64 first, u64 end) noexcept {
    return r.bytes && r.first < end && r.first + r.bytes > first;
}
bool decommit(ProgressiveSource* s, u32 b) noexcept {
    if (!bit(s->present, b) || bit(s->pinned, b))
        return false;
    for (const auto& reader : s->readers)
        if ((reader.metadata_bytes && u64{b} * kBlock < reader.metadata_first + reader.metadata_bytes &&
             block_end(*s, b) > reader.metadata_first) ||
            (reader.seek_metadata_bytes && u64{b} * kBlock < reader.seek_metadata_first + reader.seek_metadata_bytes &&
             block_end(*s, b) > reader.seek_metadata_first)) return false;
    VirtualFree(s->base + u64{b} * kBlock, kBlock, MEM_DECOMMIT);
    set_bit(s->present, b, false);
    --s->resident_blocks;
    ++s->generation;
    return true;
}
bool reader_available(void* context, u64 offset, u64 bytes) noexcept {
    auto* reader = static_cast<ProgressiveReader*>(context);
    if (!reader || !reader->source)
        return false;
    const auto& s = *reader->source;
    reader->active = true;
    reader->cursor = offset;
    const bool available = progressive_source_available(s, offset, bytes);
    reader->pending = !available;
    if (!available && range_valid(s, offset, bytes)) {
        const u32 last = static_cast<u32>((offset + maximum<u64>(bytes, 1) + kBlock - 1) / kBlock);
        reader->missing = maximum<u64>(offset, u64{first_missing(s, static_cast<u32>(offset / kBlock), last)} * kBlock);
    } else if (!available) {
        reader->missing = offset;
    }
    return available;
}
ProgressiveProbe need(const ProgressiveSource& s, u64 offset, u64 end) noexcept {
    const auto gap = gap_from(s, offset, end, kProgressiveMetadataRequestBytes);
    return {gap, gap.bytes ? ProgressiveProbeState::need_data : ProgressiveProbeState::malformed};
}
} // namespace

bool progressive_source_initialize(ProgressiveSource* s, u32 total_bytes, bool audio_buffer) noexcept {
    if (!s || !total_bytes || total_bytes > kMediaMaximumProgressiveVideoBytes)
        return false;
    *s = {};
    const u32 blocks = (total_bytes + kBlock - 1) / kBlock;
    s->base = static_cast<u8*>(VirtualAlloc(nullptr, u64{blocks} * kBlock, MEM_RESERVE, PAGE_READWRITE));
    if (!s->base)
        return false;
    s->total_bytes = total_bytes;
    s->block_count = blocks;
    s->audio_buffer = audio_buffer;
    s->ahead_bytes = audio_buffer ? 2 * kBlock : kProgressiveMinimumAheadBytes;
    progressive_source_reset_readers(s);
    return true;
}
void progressive_source_release(ProgressiveSource* s) noexcept {
    if (!s)
        return;
    if (s->base)
        VirtualFree(s->base, 0, MEM_RELEASE);
    *s = {};
}
ConstSlice<u8> progressive_source_bytes(const ProgressiveSource& s) noexcept {
    return s.base ? ConstSlice<u8>{s.base, s.total_bytes} : ConstSlice<u8>{};
}
bool progressive_source_write(ProgressiveSource* s, u64 offset, ConstSlice<u8> bytes) noexcept {
    if (!s || !bytes.data || !bytes.count || !range_valid(*s, offset, bytes.count))
        return false;
    const u64 end = offset + bytes.count;
    for (u32 b = static_cast<u32>(offset / kBlock); u64{b} * kBlock < end; ++b) {
        const u64 first = u64{b} * kBlock, last = block_end(*s, b);
        if (first < offset || last > end || bit(s->present, b))
            continue;
        // The final block may contain only a tiny clip or file tail. Windows
        // rounds this extent to pages; keep the unused reservation uncommitted.
        if (!VirtualAlloc(s->base + first, last - first, MEM_COMMIT, PAGE_READWRITE))
            return false;
        copy_bytes(s->base + first, bytes.data + (first - offset), static_cast<u32>(last - first));
        set_bit(s->present, b, true);
        ++s->resident_blocks;
        ++s->generation;
    }
    return true;
}
bool progressive_source_available(const ProgressiveSource& s, u64 offset, u64 bytes) noexcept {
    if (!range_valid(s, offset, bytes))
        return false;
    if (!bytes)
        return true;
    const u32 last = static_cast<u32>((offset + bytes + kBlock - 1) / kBlock);
    return first_missing(s, static_cast<u32>(offset / kBlock), last) == last;
}
bool progressive_source_complete(const ProgressiveSource& s) noexcept {
    return s.base && s.resident_blocks == s.block_count;
}
u32 progressive_source_payload_blocks(const ProgressiveSource& s) noexcept {
    u32 pinned_resident = 0;
    for (u32 i = 0; i < (s.block_count + 63) / 64; ++i)
        pinned_resident += static_cast<u32>(__popcnt64(s.present[i] & s.pinned[i]));
    return s.resident_blocks - pinned_resident;
}
void progressive_source_pin(ProgressiveSource* s, u64 offset, u64 bytes) noexcept {
    if (!s || !range_valid(*s, offset, bytes) || !bytes)
        return;
    const u32 last = static_cast<u32>((offset + bytes + kBlock - 1) / kBlock);
    for (u32 b = static_cast<u32>(offset / kBlock); b < last; ++b) {
        if (!bit(s->pinned, b)) {
            set_bit(s->pinned, b, true);
            ++s->pinned_blocks;
        }
    }
}
void progressive_source_set_duration(ProgressiveSource* s, u64 duration_us) noexcept {
    if (!s)
        return;
    const u64 ahead = duration_us ? u64{s->total_bytes} * kProgressiveAheadSeconds * 1'000'000ull / duration_us
                                  : kProgressiveMaximumAheadBytes;
    s->ahead_bytes = static_cast<u32>(minimum<u64>(s->audio_buffer ? 8 * kBlock : kProgressiveMaximumAheadBytes,
        maximum<u64>(s->audio_buffer ? 2 * kBlock : kProgressiveMinimumAheadBytes, ahead)));
}
Mp4SourceAvailability progressive_source_reader(ProgressiveSource* s, ProgressiveTrack track) noexcept {
    if (!s)
        return {};
    return {reader_available, &s->readers[track == ProgressiveTrack::audio ? 1 : 0]};
}
bool progressive_source_retain_metadata(ProgressiveSource* s, ProgressiveTrack track, u64 first, u32 bytes, bool seeking) noexcept {
    if (!s || (bytes && !progressive_source_available(*s, first, bytes))) return false;
    auto& reader = s->readers[track == ProgressiveTrack::audio ? 1 : 0];
    if (seeking) { reader.seek_metadata_first = bytes ? first : 0; reader.seek_metadata_bytes = bytes; }
    else { reader.metadata_first = bytes ? first : 0; reader.metadata_bytes = bytes; }
    return true;
}
void progressive_source_reset_readers(ProgressiveSource* s) noexcept {
    if (!s)
        return;
    for (auto& reader : s->readers)
        reader = {s, 0, 0, false, false};
}
u32 progressive_source_evict(ProgressiveSource* s, u32 budget_bytes) noexcept {
    if (!s || !s->base)
        return 0;
    // Leave room for one more request, or a full budget could never advance.
    const u32 request_bytes = s->audio_buffer ? kBlock : kProgressiveRequestBytes;
    const u32 behind_bytes = s->audio_buffer ? kBlock : kProgressiveBehindBytes;
    const u32 request = request_bytes / kBlock;
    const u32 budget = budget_bytes / kBlock > request ? budget_bytes / kBlock - request : 0;
    u32 payload = progressive_source_payload_blocks(*s), evicted = 0;
    if (payload <= budget)
        return 0;
    u64 window_first[2]{}, window_end[2]{}, behind_all = s->total_bytes;
    bool active[2]{};
    for (u32 i = 0; i < 2; ++i) {
        const auto& r = s->readers[i];
        if (!r.active)
            continue;
        active[i] = true;
        const u64 at = r.pending ? minimum(r.cursor, r.missing) : r.cursor;
        window_first[i] = at > behind_bytes ? at - behind_bytes : 0;
        window_end[i] = at + s->ahead_bytes + request_bytes;
        behind_all = minimum(behind_all, at);
    }
    const auto protected_block = [&](u32 b) noexcept {
        const u64 first = u64{b} * kBlock, end = block_end(*s, b);
        for (u32 i = 0; i < 2; ++i)
            if (active[i] && first < window_end[i] && end > window_first[i])
                return true;
        return false;
    };
    for (u32 b = 0; b < s->block_count && payload > budget && u64{b} * kBlock < behind_all; ++b)
        if (!protected_block(b) && decommit(s, b)) {
            --payload;
            ++evicted;
        }
    for (u32 b = s->block_count; b-- > 0 && payload > budget;)
        if (!protected_block(b) && decommit(s, b)) {
            --payload;
            ++evicted;
        }
    return evicted;
}
u32 progressive_source_trim(ProgressiveSource* s) noexcept {
    if (!s || !s->base)
        return 0;
    u32 evicted = 0;
    for (u32 b = 0; b < s->block_count; ++b)
        evicted += decommit(s, b) ? 1u : 0u;
    return evicted;
}

ProgressiveProbe progressive_mp4_probe(ProgressiveSource* s) noexcept {
    if (!s || !s->base)
        return {{}, ProgressiveProbeState::malformed};
    u64 cursor = 0;
    bool moov = false;
    for (u32 boxes = 0; cursor < s->total_bytes; ++boxes) {
        if (boxes >= kProgressiveMaximumTopLevelBoxes)
            return {{}, ProgressiveProbeState::unsupported};
        const u64 header = minimum<u64>(16, s->total_bytes - cursor);
        if (header < 8)
            return {{}, ProgressiveProbeState::malformed};
        // Moov usually trails the media data; fetch the tail in one request.
        if (!progressive_source_available(*s, cursor, header))
            return need(*s, cursor, cursor + kProgressiveMetadataRequestBytes);
        const u8* p = s->base + cursor;
        u64 size = byte_load_be32(p), header_bytes = 8;
        const u32 type = byte_load_be32(p + 4);
        if (size == 1) {
            if (header < 16)
                return {{}, ProgressiveProbeState::malformed};
            size = byte_load_be64(p + 8);
            header_bytes = 16;
        } else if (size == 0) {
            size = s->total_bytes - cursor;
        }
        if (size < header_bytes || size > s->total_bytes - cursor)
            return {{}, ProgressiveProbeState::malformed};
        progressive_source_pin(s, cursor, header_bytes);
        if (type == 0x6d6f6f66u) // moof
            return {{}, ProgressiveProbeState::unsupported};
        if (type == 0x6d6f6f76u) { // moov
            if (moov)
                return {{}, ProgressiveProbeState::malformed};
            if (size > kProgressiveMaximumMetadataBytes)
                return {{}, ProgressiveProbeState::unsupported};
            if (!progressive_source_available(*s, cursor, size))
                return need(*s, cursor, cursor + size);
            progressive_source_pin(s, cursor, size);
            moov = true;
        }
        cursor += size;
    }
    return {{}, moov ? ProgressiveProbeState::ready : ProgressiveProbeState::malformed};
}

ProgressivePlan progressive_schedule(const ProgressiveSource& s, ProgressiveRange inflight,
                                     bool read_ahead) noexcept {
    ProgressivePlan plan{};
    if (!s.base)
        return plan;
    const u32 request_bytes = s.audio_buffer ? kBlock : kProgressiveRequestBytes;
    ProgressiveRange best{};
    u64 windows_first[4]{}, windows_end[4]{};
    u32 windows = 0;
    for (const auto& r : s.readers) {
        if (!r.active || !refused(s, r))
            continue;
        windows_first[windows] = r.missing;
        windows_end[windows++] = r.missing + s.ahead_bytes;
        const auto gap = gap_from(s, r.missing, r.missing + request_bytes, request_bytes);
        if (gap.bytes && (!best.bytes || gap.first < best.first))
            best = gap;
    }
    // After a seek the other track's cursor is stale until it reads again, so
    // only refused windows decide whether the in-flight range is still useful.
    const bool any_refused = windows != 0;
    if (read_ahead && !any_refused) {
        for (const auto& r : s.readers) {
            if (!r.active || refused(s, r))
                continue;
            windows_first[windows] = r.cursor;
            windows_end[windows++] = r.cursor + s.ahead_bytes;
            const auto gap = gap_from(s, r.cursor, r.cursor + s.ahead_bytes, request_bytes);
            if (gap.bytes && (!best.bytes || gap.first < best.first))
                best = gap;
        }
    }
    if (inflight.bytes) {
        bool current = false;
        for (u32 i = 0; i < windows; ++i)
            current = current || intersects(inflight, windows_first[i], windows_end[i]);
        plan.cancel_inflight = windows && !current;
        return plan;
    }
    if (!best.bytes)
        return plan;
    // The payload budget caps every request; eviction runs before scheduling.
    const u32 payload = progressive_source_payload_blocks(s), budget = progressive_source_budget(s) / kBlock;
    if (payload >= budget)
        return plan;
    const u32 room = (budget - payload) * kBlock;
    if (best.bytes > room)
        best.bytes = room;
    plan.request = best;
    plan.submit = true;
    return plan;
}

u32 progressive_buffered_ranges(const ProgressiveSource& s, const Mp4Demuxer& video, u64 duration_us,
                                u16* begin, u16* end, u32 capacity) noexcept {
    if (!s.base || !begin || !end || !capacity || !duration_us || !video.ready || video.encoded.data != s.base)
        return 0;
    Mp4Demuxer cursor = video;
    cursor.availability = {};
    mp4_demux_reset(&cursor);
    u32 count = 0;
    bool open = false;
    i64 run_first = 0, run_end = 0;
    const auto emit = [&]() noexcept {
        const auto scale = [&](i64 t, bool up) noexcept {
            const u64 clamped = t <= 0 ? 0 : minimum(static_cast<u64>(t), duration_us);
            return static_cast<u16>((clamped * 10000ull + (up ? duration_us - 1 : 0)) / duration_us);
        };
        const u16 b = scale(run_first, false), e = scale(run_end, true);
        if (e <= b)
            return;
        if (count && b <= end[count - 1] + 1u) {
            end[count - 1] = maximum(end[count - 1], e);
        } else if (count < capacity) {
            begin[count] = b;
            end[count++] = e;
        }
    };
    while (cursor.sample_index < cursor.sample_count) {
        const auto unit = mp4_demux_next(&cursor);
        if (!unit.ok())
            break;
        const u64 offset = static_cast<u64>(unit.bytes.data - s.base);
        const i64 first = unit.presentation_time_us_signed;
        const i64 last = first + static_cast<i64>(minimum<u64>(unit.duration_us, 0x7fffffffffffull));
        if (progressive_source_available(s, offset, unit.bytes.count)) {
            run_first = open ? minimum(run_first, first) : first;
            run_end = open ? maximum(run_end, last) : last;
            open = true;
        } else if (open) {
            emit();
            open = false;
        }
    }
    if (open)
        emit();
    return count;
}

} // namespace maicord
