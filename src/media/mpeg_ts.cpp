#include "maicord/media/mpeg_ts.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
// Transport stream parsing, once per segment and no byte loop of consequence: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using E = MpegTsError;
struct Packet {
    ConstSlice<u8> payload{};
    u16 pid{};
    u8 counter{};
    bool start{}, random{}, discontinuity{}, pcr{};
};
bool stuffing(ConstSlice<u8> b) noexcept {
    for (u32 i = 0; i < b.count; ++i) if (b.data[i] != 255) return false;
    return true;
}
E packet(ConstSlice<u8> bytes, u32 offset, Packet* output) noexcept {
    if (offset > bytes.count || bytes.count - offset < 188) return E::malformed;
    const auto* p = bytes.data + offset;
    if (p[0] != 0x47 || (p[1] & 128) || !(p[3] & 48)) return E::malformed;
    if (p[3] & 192) return E::unsupported;
    Packet v{}; v.pid = static_cast<u16>(((p[1] & 31) << 8) | p[2]);
    v.start = (p[1] & 64) != 0; v.counter = p[3] & 15;
    u32 at = 4;
    if (p[3] & 32) {
        const u32 end = 5u + p[4];
        if (end > 188 || (!(p[3]&16) && end!=188) ||
            ((p[3]&16) && end==188 && ((p[1]&64) || p[5]))) return E::malformed;
        at = 5;
        if (at < end) {
            const u8 flags = p[at++]; v.random = (flags & 64) != 0;
            v.discontinuity = (flags & 128) != 0; v.pcr = (flags & 16) != 0;
            for (u8 clock = 16; clock >= 8; clock /= 2) if (flags & clock) {
                if (end - at < 6 || (p[at + 4] & 126) != 126 ||
                    (((p[at + 4] & 1) << 8) | p[at + 5]) >= 300) return E::malformed;
                at += 6;
            }
            if (flags & 4) { if (at == end) return E::malformed; ++at; }
            for (u8 field = 2; field; field /= 2) if (flags & field) {
                if (at == end) return E::malformed;
                const u32 size = p[at++]; if (size > end - at) return E::malformed;
                at += size;
            }
            if (!stuffing({p + at, end - at})) return E::malformed;
        }
        at = end;
    }
    if (p[3] & 16) v.payload = {p + at, 188 - at};
    else if (v.start) return E::malformed;
    *output = v; return E::none;
}
E continuity(ConstSlice<u8> source, u32 at, const Packet& p, u32& previous, u8& counter,
             bool& repeated, bool* duplicate) noexcept {
    *duplicate = false;
    if (p.discontinuity) return E::unsupported;
    if (previous != ~u32{0}) {
        if (!p.payload.count && !(source.data[at+3]&16)) return p.counter == counter ? E::none : E::malformed;
        if (p.counter == counter) {
            if (repeated) return E::malformed;
            const auto* old = source.data + previous; const auto* now = source.data + at;
            // Only the PCR value may change in an otherwise duplicate packet.
            for (u32 i = 0; i < 188; ++i)
                if (!(p.pcr && i >= 6 && i < 12) && old[i] != now[i]) return E::malformed;
            repeated = true; *duplicate = true; return E::none;
        }
        if (p.counter != ((counter + 1) & 15)) return E::malformed;
    }
    // Empty payload-marked stuffing packets still advance their declared
    // continuity counter. They contribute no bytes to the elementary stream.
    if (source.data[at+3]&16) { previous = at; counter = p.counter; repeated = false; }
    return E::none;
}
u32 crc(ConstSlice<u8> b) noexcept {
    u32 value = ~u32{0};
    for (u32 i = 0; i < b.count; ++i) {
        value ^= u32{b.data[i]} << 24;
        for (u32 bit = 0; bit < 8; ++bit) value = (value << 1) ^ ((value >> 31) ? 0x04c11db7u : 0);
    }
    return value;
}
u16 pid(const u8* p) noexcept { return static_cast<u16>(((p[0] & 31) << 8) | p[1]); }
u32 length(const u8* p) noexcept { return ((p[0] & 15) << 8) | p[1]; }
E descriptors(ConstSlice<u8> b) noexcept {
    for (u32 i = 0; i < b.count;) {
        if (b.count - i < 2 || b.data[i + 1] > b.count - i - 2) return E::malformed;
        if (b.data[i] == 9) return E::unsupported; // Conditional access.
        i += 2 + b.data[i + 1];
    }
    return E::none;
}
E table(ConstSlice<u8> bytes, bool pat, MpegTsProgram& out, bool& seen) noexcept {
    const auto* b = bytes.data; const u32 end = bytes.count - 4;
    if (b[0] != (pat ? 0 : 2) || (b[1] & 240) != 176 || (b[5] & 193) != 193 || crc(bytes)) return E::malformed;
    if (b[6] || b[7]) return E::unsupported; // One section per table.
    auto value = out;
    if (pat) {
        if ((end - 8) % 4) return E::malformed;
        u32 programs = 0;
        for (u32 i = 8; i < end; i += 4) {
            const u16 number = static_cast<u16>((b[i] << 8) | b[i + 1]);
            if ((b[i + 2] & 224) != 224) return E::malformed;
            if (!number) continue;
            if (++programs > 1) return E::unsupported;
            value.number = number; value.pmt_pid = pid(b + i + 2);
            if (value.pmt_pid < 16 || value.pmt_pid == kMpegTsNoPid) return E::malformed;
        }
        if (!programs) return E::malformed;
        if (seen && (value.number != out.number || value.pmt_pid != out.pmt_pid)) return E::unsupported;
    } else {
        if (end < 12 || ((b[3] << 8) | b[4]) != out.number || (b[8] & 224) != 224 || (b[10] & 240) != 240) return E::malformed;
        value.pcr_pid = pid(b + 8); value.video_pid = value.audio_pid = kMpegTsNoPid;
        if (value.pcr_pid < 16 || value.pcr_pid == value.pmt_pid) return E::malformed;
        u32 at = 12; const u32 info = length(b + 10);
        if (info > end - at) return E::malformed;
        auto error = descriptors({b + at, info}); if (error != E::none) return error;
        at += info;
        u16 used[32]{}; u32 streams = 0;
        while (at < end) {
            if (end - at < 5 || streams == 32 || (b[at + 1] & 224) != 224 || (b[at + 3] & 240) != 240) return E::malformed;
            const u16 identity = pid(b + at + 1); const u32 extra = length(b + at + 3);
            if (extra > end - at - 5 || identity < 16 || identity == kMpegTsNoPid || identity == value.pmt_pid) return E::malformed;
            for (u32 i = 0; i < streams; ++i) if (used[i] == identity) return E::malformed;
            used[streams++] = identity;
            error = descriptors({b + at + 5, extra}); if (error != E::none) return error;
            if (b[at] == 27 || b[at] == 15) {
                auto& selected = b[at] == 27 ? value.video_pid : value.audio_pid;
                if (selected != kMpegTsNoPid) return E::unsupported;
                selected = identity;
            } else if (b[at] != 21) return E::unsupported; // Timed ID3 is not media.
            at += 5 + extra;
        }
        if (value.video_pid == kMpegTsNoPid && value.audio_pid == kMpegTsNoPid) return E::unsupported;
        if (seen && (value.pcr_pid != out.pcr_pid || value.video_pid != out.video_pid || value.audio_pid != out.audio_pid)) return E::unsupported;
    }
    out = value; seen = true; return E::none;
}
struct Section {
    u8 bytes[1024]{};
    u32 used{}, wanted{}, previous{~u32{0}};
    u8 counter{};
    bool duplicated{}, seen{};
};
E section_bytes(Section& s, ConstSlice<u8> bytes, bool pat, MpegTsProgram& value, bool fresh) noexcept {
    u32 at = 0;
    while (at < bytes.count) {
        if (!s.used && (!fresh || bytes.data[at] == 255))
            return stuffing({bytes.data + at, bytes.count - at}) ? E::none : E::malformed;
        const u32 count = minimum(bytes.count - at, (s.wanted ? s.wanted : 3) - s.used);
        copy_bytes(s.bytes + s.used, bytes.data + at, count); s.used += count; at += count;
        if (s.used == 3 && !s.wanted) {
            s.wanted = 3 + length(s.bytes + 1);
            if (s.wanted < 12 || s.wanted > sizeof(s.bytes)) return E::malformed;
        }
        if (s.used == s.wanted) {
            const auto error = table({s.bytes, s.used}, pat, value, s.seen);
            if (error != E::none) return error;
            s.used = s.wanted = 0;
        }
    }
    return E::none;
}
E section(Section& s, ConstSlice<u8> source, u32 at, const Packet& p, bool pat, MpegTsProgram& value) noexcept {
    bool duplicate{};
    auto error = continuity(source, at, p, s.previous, s.counter, s.duplicated, &duplicate);
    if (error != E::none || duplicate || !p.payload.count) return error;
    auto bytes = p.payload;
    if (p.start) {
        const u32 prefix = bytes.data[0];
        if (prefix >= bytes.count) return E::malformed;
        error = section_bytes(s, {bytes.data + 1, prefix}, pat, value, false);
        if (error != E::none || s.used) return error == E::none ? E::malformed : error;
        bytes = {bytes.data + 1 + prefix, bytes.count - 1 - prefix};
    } else if (!s.used) return E::malformed;
    return section_bytes(s, bytes, pat, value, p.start);
}
bool input(ConstSlice<u8> b) noexcept {
    return b.data && b.count && b.count <= kMpegTsMaximumBytes && b.count % 188 == 0 && byte_range_valid(b);
}
bool stamp(const u8* b, u8 prefix, u64* result) noexcept {
    if ((b[0] & 241) != (prefix | 1) || !(b[2] & 1) || !(b[4] & 1)) return false;
    *result = (u64{(b[0] >> 1) & 7u} << 30) | (u64{b[1]} << 22) |
        ((u64{b[2]} >> 1) << 15) | (u64{b[3]} << 7) | (b[4] >> 1);
    return true;
}
} // namespace
MpegTsError mpeg_ts_program(ConstSlice<u8> b, MpegTsProgram* output) noexcept {
    if (!input(b) || !output || !byte_range_valid(output, sizeof(*output)) ||
        byte_ranges_overlap(b.data, b.count, output, sizeof(*output))) return E::invalid_input;
    MpegTsProgram value{}; Section pat{}, pmt{}; bool clock = false;
    for (u32 at = 0; at < b.count; at += 188) {
        Packet p{}; auto error = packet(b, at, &p); if (error != E::none) return error;
        if (!p.pid) error = section(pat, b, at, p, true, value);
        else if (pat.seen && p.pid == value.pmt_pid) error = section(pmt, b, at, p, false, value);
        else if (pmt.seen) {
            if (p.pid == value.pcr_pid && p.pcr) clock = true;
            if (p.pid == value.pcr_pid || p.pid == value.video_pid || p.pid == value.audio_pid) {
                if (p.discontinuity) return E::unsupported;
            }
        }
        if (error != E::none) return error;
    }
    // Some separately delivered HLS tracks explicitly declare no PCR PID.
    // Their PES timestamps remain mandatory and authoritative in the reader.
    if (!pat.seen || !pmt.seen || pat.used || pmt.used || (!clock && value.pcr_pid!=kMpegTsNoPid)) return E::malformed;
    *output = value; return E::none;
}
MpegTsError mpeg_ts_next(MpegTsReader* reader, Slice<u8> scratch, MpegTsPes* output) noexcept {
    if (!reader || !byte_range_valid(reader, sizeof(*reader)) || !input(reader->source) ||
        reader->offset > reader->source.count || reader->offset % 188 || reader->pid < 16 || reader->pid >= kMpegTsNoPid ||
        (reader->previous != ~u32{0} && (reader->previous >= reader->offset || reader->previous % 188)) ||
        !scratch.data || !scratch.count || !output) return E::invalid_input;
    const ConstSlice<u8> spans[]{reader->source, {scratch.data, scratch.count},
        {reinterpret_cast<const u8*>(reader), sizeof(*reader)}, {reinterpret_cast<const u8*>(output), sizeof(*output)}};
    if (!byte_ranges_all_disjoint({spans, 4})) return E::invalid_input;
    auto next = *reader; u32 used = 0; bool started = false, random = false;
    while (next.offset < next.source.count) {
        Packet p{}; auto error = packet(next.source, next.offset, &p); if (error != E::none) return error;
        if (p.pid == next.pid) {
            // A same-counter duplicate can repeat the PES start flag.
            if (started && p.start && p.counter != next.counter) break;
            bool duplicate{};
            error = continuity(next.source, next.offset, p, next.previous, next.counter, next.duplicated, &duplicate);
            if (error != E::none) return error;
            if (!duplicate && p.payload.count) {
                if (!started) { if (!p.start) return E::malformed; started = true; random = p.random; }
                if (p.payload.count > scratch.count - used) return E::capacity;
                copy_bytes(scratch.data + used, p.payload.data, p.payload.count); used += p.payload.count;
            }
        }
        next.offset += 188;
    }
    if (!started) return E::end_of_stream;
    const auto* b = scratch.data;
    if (used < 14 || b[0] || b[1] || b[2] != 1 || (b[6] & 192) != 128 ||
        !((b[3] >= 192 && b[3] <= 239))) return E::malformed;
    if ((b[6] & 48) || (b[7] & 63)) return E::unsupported;
    const u32 declared = (b[4] << 8) | b[5], header = 9u + b[8];
    if ((declared && declared + 6 != used) || (!declared && b[3] < 224) || header >= used) return E::malformed;
    const u8 flags = b[7] >> 6; const u32 timestamps = flags == 3 ? 10 : 5;
    MpegTsPes value{}; value.random_access = random;
    if (flags < 2 || b[8] < timestamps || !stamp(b + 9, flags == 3 ? 48 : 32, &value.pts)) return E::malformed;
    value.dts = value.pts;
    if (flags == 3 && !stamp(b + 14, 16, &value.dts)) return E::malformed;
    if (!stuffing({b + 9 + timestamps, b[8] - timestamps})) return E::malformed;
    value.payload = {b + header, used - header};
    *reader = next; *output = value; return E::none;
}
bool mpeg_ts_unwrap(u64 raw, u64 reference, u64* output) noexcept {
    constexpr u64 wrap = u64{1} << 33, half = wrap / 2, mask = wrap - 1;
    if (!output || !byte_range_valid(output, sizeof(*output)) || raw >= wrap) return false;
    const u64 forward = (raw - (reference & mask)) & mask;
    if (forward == half) return false;
    if (forward < half) { if (reference > ~u64{0} - forward) return false; *output = reference + forward; }
    else { const u64 back = wrap - forward; if (back > reference) return false; *output = reference - back; }
    return true;
}
} // namespace maicord
#pragma clang attribute pop
