#include "maicord/media/video/rtp_video_packetize.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/av1.hpp"

namespace maicord {
u32 annexb_next_unit(ConstSlice<u8> data, u32 from, u32* start) noexcept {
    // Nothing begins a start code at i, i + 1 or i + 2 while the third byte is above one.
    for (u32 i = from; i + 2 < data.count;) {
        if (data[i + 2] > 1) {
            i += 3;
            continue;
        }
        if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
            *start = i > from && !data[i - 1] ? i - 1 : i;
            return i + 3;
        }
        ++i;
    }
    *start = data.count;
    return data.count;
}
u32 leb128_bytes(u32 value) noexcept {
    u32 bytes = 1;
    for (; value >= 0x80; value >>= 7)
        ++bytes;
    return bytes;
}
u32 leb128_write(u8* output, u32 value) noexcept {
    u32 n = 0;
    do {
        output[n++] = static_cast<u8>((value & 0x7f) | (value >= 0x80 ? 0x80 : 0));
        value >>= 7;
    } while (value);
    return n;
}
namespace {
constexpr u8 kStapA = 24, kFuA = 28, kH265Aggregation = 48, kH265Fragmentation = 49;
constexpr u32 kAv1MaximumElements = 63; // The depacketizer reads at most 64 per packet.

bool h265(const RtpVideoPacketizer& p) noexcept {
    return p.codec == RtpVideoPacketCodec::h265;
}
u32 nal_header_bytes(const RtpVideoPacketizer& p) noexcept {
    return h265(p) ? 2 : 1;
}
// Finds the unit at or after from into p; false when there is none (or the frame is malformed: failed).
bool locate(RtpVideoPacketizer& p, u32 from) noexcept {
    const auto& frame = p.frame;
    if (p.codec != RtpVideoPacketCodec::av1) {
        u32 start = 0;
        const u32 begin = annexb_next_unit(frame, from, &start);
        if (begin >= frame.count)
            return false;
        u32 end = 0;
        annexb_next_unit(frame, begin, &end);
        if (end - begin < nal_header_bytes(p) || (frame[begin] & 0x80)) {
            p.failed = true;
            return false;
        }
        p.unit = begin;
        p.payload = begin + nal_header_bytes(p);
        p.unit_end = p.next_unit = end;
        return true;
    }
    for (u32 at = from; at < frame.count;) {
        Av1Obu obu;
        u32 next = at;
        if (av1_next_obu(frame, &next, &obu) != Av1Status::ok) {
            p.failed = true;
            return false;
        }
        // A temporal delimiter, tile list or padding OBU is not sent.
        if (obu.type == 2 || obu.type == 8 || obu.type == 15) {
            at = next;
            continue;
        }
        p.unit = at;
        p.payload = static_cast<u32>(obu.payload.data - frame.data);
        p.unit_end = p.next_unit = next;
        return true;
    }
    return false;
}
// Moves to the unit after the current one; at the end the cursor rests at frame.count.
void advance(RtpVideoPacketizer& p) noexcept {
    p.sent = p.fragments = p.fragment = 0;
    if (!locate(p, p.next_unit))
        p.unit = p.payload = p.unit_end = p.next_unit = p.frame.count;
}
bool done(const RtpVideoPacketizer& p) noexcept {
    return p.unit >= p.frame.count;
}

u32 h26x_fragment(RtpVideoPacketizer& p, u8* out) noexcept {
    const u8* unit = p.frame.data + p.unit;
    const u32 total = p.unit_end - p.payload;
    const u32 bytes = total / p.fragments + (p.fragment < total % p.fragments ? 1 : 0);
    const bool start = p.fragment == 0, end = p.fragment + 1 == p.fragments;
    u32 at = 0;
    if (h265(p)) {
        out[at++] = static_cast<u8>((unit[0] & 0x81) | (kH265Fragmentation << 1));
        out[at++] = unit[1];
        out[at++] = static_cast<u8>((start ? 0x80 : 0) | (end ? 0x40 : 0) | ((unit[0] >> 1) & 0x3f));
    } else {
        out[at++] = static_cast<u8>((unit[0] & 0xe0) | kFuA);
        out[at++] = static_cast<u8>((start ? 0x80 : 0) | (end ? 0x40 : 0) | (unit[0] & 0x1f));
    }
    copy_bytes(out + at, p.frame.data + p.payload + p.sent, bytes);
    p.sent += bytes;
    ++p.fragment;
    if (end)
        advance(p);
    return at + bytes;
}

u32 h26x_next(RtpVideoPacketizer& p, u8* out) noexcept {
    if (p.fragments)
        return h26x_fragment(p, out);
    const u32 size = p.unit_end - p.unit;
    if (size > p.max_payload) {
        const u32 overhead = h265(p) ? 3 : 2, per = p.max_payload - overhead, total = p.unit_end - p.payload;
        p.fragments = (total + per - 1) / per;
        if (p.fragments < 2)
            p.fragments = 2;
        return h26x_fragment(p, out);
    }
    // Aggregate the following small units while they fit (STAP-A / AP).
    const u32 header = nal_header_bytes(p);
    u32 total = header + 2 + size, units = 1;
    // The aggregate's header takes the highest NRI and the lowest TID of its units.
    const u8* first = p.frame.data + p.unit;
    u32 nri = first[0] & 0x60u, tid = h265(p) ? first[1] & 7u : 7u;
    RtpVideoPacketizer peek = p;
    while (locate(peek, peek.next_unit) && total + 2 + (peek.unit_end - peek.unit) <= p.max_payload) {
        total += 2 + (peek.unit_end - peek.unit);
        ++units;
        const u8* unit = p.frame.data + peek.unit;
        nri = maximum<u32>(nri, unit[0] & 0x60u);
        if (h265(p))
            tid = minimum<u32>(tid, unit[1] & 7u);
    }
    if (peek.failed) {
        p.failed = true;
        return 0;
    }
    if (units == 1) {
        copy_bytes(out, p.frame.data + p.unit, size);
        advance(p);
        return size;
    }
    u32 at = 0;
    if (h265(p)) {
        out[at++] = static_cast<u8>(kH265Aggregation << 1);
        out[at++] = static_cast<u8>(tid);
    } else
        out[at++] = static_cast<u8>(nri | kStapA);
    for (u32 i = 0; i != units; ++i) {
        const u32 bytes = p.unit_end - p.unit;
        out[at++] = static_cast<u8>(bytes >> 8);
        out[at++] = static_cast<u8>(bytes);
        copy_bytes(out + at, p.frame.data + p.unit, bytes);
        at += bytes;
        advance(p);
    }
    return at;
}

// AV1 element bytes: the OBU header without obu_has_size_field, its extension, the payload.
u32 element_bytes(const RtpVideoPacketizer& p) noexcept {
    return ((p.frame[p.unit] & 4) ? 2 : 1) + (p.unit_end - p.payload);
}
void element_copy(const RtpVideoPacketizer& p, u8* out, u32 from, u32 bytes) noexcept {
    const u32 header = (p.frame[p.unit] & 4) ? 2 : 1;
    for (; bytes && from < header; --bytes, ++from)
        *out++ = from ? p.frame[p.unit + 1] : static_cast<u8>(p.frame[p.unit] & ~2u);
    copy_bytes(out, p.frame.data + p.payload + (from - header), bytes);
}
u32 av1_next(RtpVideoPacketizer& p, u8* out) noexcept {
    const bool continues = p.sent != 0;
    bool more = false;
    u32 at = 1, elements = 0;
    while (!done(p) && elements != kAv1MaximumElements) {
        const u32 remaining = element_bytes(p) - p.sent, room = p.max_payload - at;
        if (leb128_bytes(remaining) + remaining <= room) {
            at += leb128_write(out + at, remaining);
            element_copy(p, out + at, p.sent, remaining);
            at += remaining;
            ++elements;
            advance(p);
            continue;
        }
        // A fragment: worth starting only with a useful part of the packet left.
        if (elements && room < 64)
            break;
        u32 take = room - leb128_bytes(room);
        if (!take)
            break;
        at += leb128_write(out + at, take);
        element_copy(p, out + at, p.sent, take);
        at += take;
        p.sent += take;
        ++elements;
        more = true;
        break;
    }
    out[0] = static_cast<u8>((continues ? 0x80 : 0) | (more ? 0x40 : 0) | (p.first && p.keyframe ? 0x08 : 0));
    return at;
}
} // namespace

bool rtp_packetizer_begin(RtpVideoPacketizer* packetizer, RtpVideoPacketCodec codec, ConstSlice<u8> frame,
                          bool keyframe, u32 max_payload) noexcept {
    if (!packetizer || !frame.data || !frame.count || max_payload < 64)
        return false;
    reset_value(packetizer);
    auto& p = *packetizer;
    p.frame = frame;
    p.codec = codec;
    p.keyframe = keyframe;
    p.max_payload = max_payload;
    p.first = true;
    // Validate every unit before anything is sent.
    RtpVideoPacketizer walk = p;
    u32 units = 0;
    for (u32 from = 0; locate(walk, from); from = walk.next_unit)
        ++units;
    if (walk.failed || !units || !locate(p, 0))
        return false;
    return true;
}

u32 rtp_packetizer_next(RtpVideoPacketizer* packetizer, Slice<u8> output, bool* marker) noexcept {
    auto& p = *packetizer;
    if (p.failed || done(p) || output.count < p.max_payload)
        return 0;
    const u32 bytes = p.codec == RtpVideoPacketCodec::av1 ? av1_next(p, output.data) : h26x_next(p, output.data);
    p.first = false;
    *marker = done(p) && !p.fragments && !p.sent;
    return p.failed ? 0 : bytes;
}
} // namespace maicord
