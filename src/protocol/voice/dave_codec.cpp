#include "maicord/protocol/voice/dave_codec.hpp"
#include "maicord/core/memory.hpp"

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
// The next start code prefix at or after from: *start is where its zeros
// begin (a fourth leading zero included), the return value where the NAL
// unit after it begins; count when there is none.
u32 next_nal(const u8* data, u32 count, u32 from, u32* start) noexcept {
    for (u32 i = from; i + 2 < count; ++i)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1) {
            *start = i > from && !data[i - 1] ? i - 1 : i;
            return i + 3;
        }
    *start = count;
    return count;
}
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
    u32 nal = next_nal(frame.data, frame.count, 0, &start);
    if (start != 0 && !(start == 1 && !frame[0])) {
        b.ok = false; // Annex B starts with a start code.
        return;
    }
    while (b.ok && nal < frame.count) {
        u32 end = 0;
        const u32 following = next_nal(frame.data, frame.count, nal, &end);
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
bool read_leb128(ConstSlice<u8> data, u32* at, u64* value) noexcept {
    *value = 0;
    for (u32 i = 0; i != 8 && *at < data.count; ++i) {
        const u8 byte = data[(*at)++];
        *value |= u64{byte & 0x7fu} << (7 * i);
        if (!(byte & 0x80))
            return true;
    }
    return false;
}
struct Obu {
    u32 header, payload, size; // Offsets of the header and payload, payload bytes.
    u8 type;
    bool extension, kept;
};
// The OBU at *at, which moves past it; false when the frame is malformed.
bool next_obu(ConstSlice<u8> frame, u32* at, Obu* obu) noexcept {
    const u8 first = frame[*at];
    *obu = {*at, 0, 0, static_cast<u8>((first >> 3) & 15), (first & 4) != 0, false};
    if (first & 0x80)
        return false;
    *at += obu->extension ? 2 : 1;
    u64 size = frame.count - (*at < frame.count ? *at : frame.count);
    if (*at > frame.count || ((first & 2) && !read_leb128(frame, at, &size)) || size > frame.count - *at)
        return false;
    obu->payload = *at;
    obu->size = static_cast<u32>(size);
    obu->kept = obu->type != 2 && obu->type != 8 && obu->type != 15; // TD, tile list, padding.
    *at += obu->size;
    return true;
}
void prepare_av1(Builder& b, ConstSlice<u8> frame) noexcept {
    Obu obu;
    u32 last = ~u32{0};
    for (u32 at = 0; at < frame.count;) {
        if (!next_obu(frame, &at, &obu)) {
            b.ok = false;
            return;
        }
        if (obu.kept)
            last = obu.header;
    }
    b.ok = last != ~u32{0};
    for (u32 at = 0; b.ok && at < frame.count;) {
        next_obu(frame, &at, &obu);
        if (!obu.kept)
            continue;
        const bool final = obu.header == last;
        const u8 header = static_cast<u8>(final ? frame[obu.header] & ~2u : frame[obu.header] | 2u);
        b.clear(&header, 1);
        if (obu.extension)
            b.clear(frame.data + obu.header + 1, 1);
        if (!final) {
            u8 size[8];
            u32 n = 0;
            u32 value = obu.size;
            do {
                size[n] = static_cast<u8>((value & 0x7f) | (value >= 0x80 ? 0x80 : 0));
                value >>= 7;
                ++n;
            } while (value);
            b.clear(size, n);
        }
        b.encrypted(frame.data + obu.payload, obu.size);
    }
}
bool has_start_code(ConstSlice<u8> data, u32 from, u32 to) noexcept {
    for (u32 i = from; i + 2 < to; ++i)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1)
            return true;
    return false;
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
