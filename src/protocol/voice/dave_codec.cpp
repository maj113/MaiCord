#include "maicord/protocol/voice/dave_codec.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/rtp_video_packetize.hpp"

namespace maicord {
namespace {
struct Builder {
    Slice<u8> output;
    Slice<DaveClearRange> ranges;
    u32 bytes, count;
    bool ok;
    bool room(u32 n) noexcept {
        ok = ok && n <= output.count - bytes;
        return ok;
    }
    void encrypted(const u8* data, u32 n) noexcept {
        if (!room(n))
            return;
        copy_bytes(output.data + bytes, data, n);
        bytes += n;
    }
    void clear(const u8* data, u32 n) noexcept {
        if (!n || !room(n))
            return;
        if (count && ranges[count - 1].offset + ranges[count - 1].bytes == bytes)
            ranges[count - 1].bytes += n;
        else if (count < ranges.count)
            ranges[count++] = {bytes, n};
        else {
            ok = false;
            return;
        }
        copy_bytes(output.data + bytes, data, n);
        bytes += n;
    }
};
// Bytes (after the NAL header) holding first_mb_in_slice, slice_type and
// pic_parameter_set_id, three ue(v); 0 when the unit is too short.
u32 h264_slice_header_bytes(const u8* data, u32 count) noexcept {
    u32 bit = 0;
    const auto read = [&]() noexcept -> i32 {
        return bit < count * 8 ? (data[bit >> 3] >> (7 - (bit & 7))) & 1 : -1;
    };
    for (u32 value = 0; value != 3; ++value) {
        u32 zeros = 0;
        for (;;) {
            const i32 b = read();
            ++bit;
            if (b < 0 || zeros > 31)
                return 0;
            if (b)
                break;
            ++zeros;
        }
        bit += zeros;
        if (bit > count * 8)
            return 0;
    }
    return (bit + 7) / 8;
}
void prepare_h26x(Builder& b, bool h265, ConstSlice<u8> frame) noexcept {
    static constexpr u8 kStartCode[]{0, 0, 0, 1};
    u32 start = 0;
    u32 nal = annexb_next_unit(frame, 0, &start);
    if (start != 0 && !(start == 1 && !frame[0])) {
        b.ok = false; // Annex B starts with a start code.
        return;
    }
    while (b.ok && nal < frame.count) {
        u32 end = 0;
        const u32 following = annexb_next_unit(frame, nal, &end);
        const u8* unit = frame.data + nal;
        const u32 size = end - nal, header = h265 ? 2u : 1u;
        b.clear(kStartCode, 4);
        if (size < header) {
            b.ok = false;
            return;
        }
        const u8 type = h265 ? (unit[0] >> 1) & 0x3f : unit[0] & 0x1f;
        u32 clear = size;
        if (h265 && type < 32)
            clear = header;
        else if (!h265 && (type == 1 || type == 5)) {
            const u32 slice = h264_slice_header_bytes(unit + 1, size - 1);
            if (!slice) {
                b.ok = false;
                return;
            }
            clear = header + slice;
        }
        b.clear(unit, clear);
        b.encrypted(unit + clear, size - clear);
        nal = following;
    }
}
// Temporal delimiters, tile lists and padding are dropped.
bool kept(u8 type) noexcept {
    return type != 2 && type != 8 && type != 15;
}
void prepare_av1(Builder& b, ConstSlice<u8> frame) noexcept {
    Av1Obu obu;
    u32 last = ~u32{0};
    for (u32 at = 0; at < frame.count;) {
        const u32 header = at;
        if (av1_next_obu(frame, &at, &obu) != Av1Status::ok) {
            b.ok = false;
            return;
        }
        if (kept(obu.type))
            last = header;
    }
    b.ok = last != ~u32{0};
    for (u32 at = 0; b.ok && at < frame.count;) {
        const u32 header = at;
        av1_next_obu(frame, &at, &obu);
        if (!kept(obu.type))
            continue;
        const bool final = header == last;
        const u8 first = static_cast<u8>(final ? frame[header] & ~2u : frame[header] | 2u);
        b.clear(&first, 1);
        if (obu.extension)
            b.clear(frame.data + header + 1, 1);
        if (!final) {
            u8 size[5];
            b.clear(size, leb128_write(size, obu.payload.count));
        }
        b.encrypted(obu.payload.data, obu.payload.count);
    }
}
bool has_start_code(ConstSlice<u8> data, u32 from, u32 to) noexcept {
    u32 start = 0;
    annexb_next_unit({data.data, to}, from, &start);
    return start != to;
}
} // namespace

DavePreparedFrame dave_frame_prepare(DaveVideoCodec codec, ConstSlice<u8> frame, Slice<u8> output,
                                     Slice<DaveClearRange> ranges) noexcept {
    if (!frame.data || !frame.count || !output.data || !ranges.data)
        return {};
    Builder b{output, ranges, 0, 0, true};
    if (codec == DaveVideoCodec::av1)
        prepare_av1(b, frame);
    else
        prepare_h26x(b, codec == DaveVideoCodec::h265, frame);
    if (!b.ok || !b.bytes)
        return {};
    return {b.bytes, b.count};
}

bool dave_frame_start_code_free(ConstSlice<u8> sealed, ConstSlice<DaveClearRange> ranges) noexcept {
    u32 encrypted = 0;
    for (u32 i = 0; i != ranges.count; ++i) {
        const auto& range = ranges[i];
        if (range.offset > encrypted &&
            has_start_code(sealed, encrypted >= 2 ? encrypted - 2 : 0,
                           range.offset + 2 < sealed.count ? range.offset + 2 : sealed.count))
            return false;
        encrypted = range.offset + range.bytes;
    }
    return encrypted >= sealed.count || !has_start_code(sealed, encrypted >= 2 ? encrypted - 2 : 0, sealed.count);
}
} // namespace maicord
