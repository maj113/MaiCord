#include "maicord/platform/desktop/clipboard_image.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/sys/simd_haswell.hpp"
#if defined(_WIN32)
#include "maicord/sys/gdi.hpp"
#endif

namespace maicord {
namespace {
constexpr u32 kStoredBlock = 65535;
constexpr u8 kPngSignature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
bool extent_valid(u32 width, u32 height) noexcept {
    return width && height && width <= kClipboardImageMaximumExtent && height <= kClipboardImageMaximumExtent;
}
// A chunk at out: length, type, data (already in place when data is out + 8) and CRC; returns its end. A chunk holds
// at most a 64-megapixel image's IDAT (about 256 MiB), so the CRC's 32-bit count covers it.
u8* chunk(u8* out, const char* type, const u8* data, u32 bytes) noexcept {
    byte_store_be32(out, bytes);
    copy_bytes(out + 4, type, 4);
    if (bytes && data != out + 8) copy_bytes(out + 8, data, bytes);
    byte_store_be32(out + 8 + bytes, crc32_extend(0, out + 4, 4 + bytes));
    return out + 12 + bytes;
}
// Signature and IHDR (8-bit, no interlace); returns where the IDAT chunk starts.
u8* write_header(u8* out, u32 width, u32 height, u8 color_type) noexcept {
    copy_bytes(out, kPngSignature, 8);
    u8 header[13]{};
    byte_store_be32(header, width);
    byte_store_be32(header + 4, height);
    header[8] = 8;
    header[9] = color_type;
    return chunk(out + 8, "IHDR", header, 13);
}
// The five bytes that open a stored deflate block of `block` bytes.
void store_header(u8* at, u32 block, bool last) noexcept {
    at[0] = last ? 1 : 0;
    at[1] = static_cast<u8>(block);
    at[2] = static_cast<u8>(block >> 8);
    at[3] = static_cast<u8>(~block);
    at[4] = static_cast<u8>(~block >> 8);
}
u64 raw_bytes(u32 width, u32 height) noexcept {
    return u64{height} * (1 + u64{width} * 4);
}
u64 zlib_bytes(u64 raw) noexcept {
    const u64 blocks = raw ? (raw + kStoredBlock - 1) / kStoredBlock : 1;
    return 2 + raw + 5 * blocks + 4;
}
// Adds `count` bytes to the two Adler-32 sums, which stay below 65521. A block of 32 bytes adds the sum of its bytes
// to the first sum. It adds 32 times the first sum and the bytes with the weights 32 to 1 to the second sum. At most
// 173 blocks go between two reductions, so that no lane and no sum wraps.
MAICORD_HASWELL void adler32_update(u32* sum_a, u32* sum_b, const u8* data, u64 count) noexcept {
    static constexpr u8 kWeights[32]{32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
                                     16, 15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1};
    const __m256i weights = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(kWeights));
    const __m256i zero = _mm256_setzero_si256(), ones = _mm256_set1_epi16(1);
    u64 a = *sum_a, b = *sum_b;
    while (count >= 32) {
        const u32 blocks = static_cast<u32>(minimum<u64>(count / 32, 173));
        __m256i bytes = zero, before = zero, weighted = zero;
        for (u32 i = 0; i != blocks; ++i, data += 32) {
            const __m256i block = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(data));
            before = _mm256_add_epi64(before, bytes);
            bytes = _mm256_add_epi64(bytes, _mm256_sad_epu8(block, zero));
            weighted = _mm256_add_epi32(weighted, _mm256_madd_epi16(_mm256_maddubs_epi16(block, weights), ones));
        }
        u64 wide[12];
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide), bytes);
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide + 4), before);
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(wide + 8), weighted);
        b += u64{blocks} * 32 * a + 32 * (wide[4] + wide[5] + wide[6] + wide[7]);
        for (u32 i = 8; i != 12; ++i) b += (wide[i] & 0xffffffffu) + (wide[i] >> 32);
        a = (a + wide[0] + wide[1] + wide[2] + wide[3]) % 65521;
        b %= 65521;
        count -= u64{blocks} * 32;
    }
    for (; count; --count) { a += *data++; b += a; }
    *sum_a = static_cast<u32>(a % 65521);
    *sum_b = static_cast<u32>(b % 65521);
}
} // namespace
u64 clipboard_image_png_bytes(u32 width, u32 height) noexcept {
    if (!extent_valid(width, height))
        return 0;
    return 8 + 25 + 12 + zlib_bytes(raw_bytes(width, height)) + 12;
}
u64 clipboard_image_encode_png(const u8* rgba, u32 width, u32 height, u32 stride, Slice<u8> output) noexcept {
    const u64 total = clipboard_image_png_bytes(width, height);
    if (!rgba || !total || stride < width * 4 || !output.data || output.count < total)
        return 0;
    u8* idat = write_header(output.data, width, height, 6); // truecolor with alpha
    const u64 raw = raw_bytes(width, height), zlib = zlib_bytes(raw);
    u8* z = idat + 8;
    z[0] = 0x78; // deflate, 32 KiB window
    z[1] = 0x01; // no preset dictionary, fastest; 0x7801 % 31 == 0
    // The raw stream is a zero filter byte and the RGBA bytes for each row. It goes into stored blocks of 65535 bytes.
    u64 cursor = 2, left = raw;
    u32 room = 0, adler_a = 1, adler_b = 0;
    const u8 no_filter = 0;
    for (u32 row = 0; row < height; ++row) {
        const ConstSlice<u8> parts[2]{{&no_filter, 1}, {rgba + u64{row} * stride, width * 4}};
        for (const ConstSlice<u8>& part : parts) {
            adler32_update(&adler_a, &adler_b, part.data, part.count);
            for (u32 done = 0; done != part.count;) {
                if (!room) {
                    room = static_cast<u32>(minimum<u64>(kStoredBlock, left));
                    store_header(z + cursor, room, room == left);
                    cursor += 5;
                }
                const u32 count = minimum(part.count - done, room);
                copy_bytes(z + cursor, part.data + done, count);
                cursor += count; done += count; room -= count; left -= count;
            }
        }
    }
    byte_store_be32(z + cursor, (adler_b << 16) | adler_a);
    u8* iend = chunk(idat, "IDAT", z, static_cast<u32>(zlib));
    chunk(iend, "IEND", nullptr, 0);
    return total;
}
// The DIB formats and the paste conversion are Windows' (the Wayland backend pastes PNG as it receives it).
#if defined(_WIN32)
// A 32-bit pixel with its bytes 0 and 2 exchanged: RGBA to BGRA and back.
static u32 swap_red_blue(u32 pixel) noexcept {
    const u32 outer = pixel & 0x00ff00ffu;
    return (pixel & 0xff00ff00u) | (outer << 16) | (outer >> 16);
}
u64 clipboard_image_dib_bytes(u32 width, u32 height) noexcept {
    return extent_valid(width, height) ? sizeof(BITMAPV5HEADER) + u64{width} * height * 4 : 0;
}
u64 clipboard_image_build_dib(const u8* rgba, u32 width, u32 height, u32 stride, Slice<u8> output) noexcept {
    const u64 total = clipboard_image_dib_bytes(width, height);
    if (!rgba || !total || stride < width * 4 || !output.data || output.count < total)
        return 0;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = static_cast<LONG>(height); // bottom-up: the most widely accepted form
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(total - sizeof(header));
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    copy_bytes(output.data, &header, sizeof(header));
    u8* out = output.data + sizeof(header);
    for (u32 y = 0; y < height; ++y) {
        const u8* source = rgba + u64{height - 1 - y} * stride;
        u8* target = out + u64{y} * width * 4;
        for (u32 x = 0; x < width; ++x)
            byte_store_le32(target + u64{x} * 4, swap_red_blue(byte_load_le32(source + u64{x} * 4)));
    }
    return total;
}

namespace {
// ---- Paste conversion -------------------------------------------------------
void unreserve(void* memory) noexcept { if (memory) pages_release(memory); }
struct Channel { u32 mask, shift, bits; };
Channel channel(u32 mask) noexcept {
    Channel c{mask, 0, 0};
    if (!mask) return c;
    while (!((mask >> c.shift) & 1)) ++c.shift;
    while (c.shift + c.bits < 32 && ((mask >> (c.shift + c.bits)) & 1)) ++c.bits;
    return c;
}
u8 extract(u32 pixel, Channel c) noexcept {
    if (!c.bits) return 0;
    const u32 value = (pixel & c.mask) >> c.shift;
    if (c.bits >= 8) return static_cast<u8>(value >> (c.bits - 8));
    return static_cast<u8>(value * 255u / ((1u << c.bits) - 1u));
}
struct Dib {
    const u8* pixels;
    const u8* palette;
    u32 palette_count, width, height, stride, bits;
    bool top_down;
    Channel red, green, blue, alpha;
    bool use_alpha;
    bool plain; // 32 bits with the bytes blue, green, red and alpha or an unused byte: the BI_RGB layout
};
bool dib_layout(ConstSlice<u8> dib, Dib* out) noexcept {
    reset_value(out);
    if (!dib.data || dib.count < 40) return false;
    const u32 header = byte_load_le32(dib.data);
    const i32 width = static_cast<i32>(byte_load_le32(dib.data + 4)), height = static_cast<i32>(byte_load_le32(dib.data + 8));
    const u32 planes = u32{dib.data[12]} | u32{dib.data[13]} << 8, bits = u32{dib.data[14]} | u32{dib.data[15]} << 8;
    const u32 compression = byte_load_le32(dib.data + 16), used = byte_load_le32(dib.data + 32);
    if (header < 40 || header > dib.count || planes != 1 || width <= 0 || !height || height == (-2147483647 - 1)) return false;
    const u32 w = static_cast<u32>(width), h = static_cast<u32>(height < 0 ? -height : height);
    if (w > kClipboardPasteMaximumExtent || h > kClipboardPasteMaximumExtent || u64{w} * h > kClipboardPasteMaximumPixels) return false;
    constexpr u32 rgb = 0, bitfields = 3, alphabitfields = 6;
    u64 offset = header;
    u32 masks[4]{};
    if (bits == 16 || bits == 32) {
        if (compression == bitfields || compression == alphabitfields) {
            const u32 count = compression == alphabitfields ? 4 : 3;
            if (header >= 40 + count * 4) {
                // V2+ headers (52/56/108/124 bytes) hold the masks themselves.
                for (u32 i = 0; i < 4 && 40 + i * 4 + 4 <= header; ++i) masks[i] = byte_load_le32(dib.data + 40 + i * 4);
            } else {
                // A BITMAPINFOHEADER carries its masks after the header.
                if (offset + count * 4 > dib.count) return false;
                for (u32 i = 0; i < count; ++i) masks[i] = byte_load_le32(dib.data + offset + i * 4);
                offset += count * 4;
            }
        } else if (compression == rgb) {
            if (bits == 16) { masks[0] = 0x7c00; masks[1] = 0x03e0; masks[2] = 0x001f; }
            else { masks[0] = 0x00ff0000; masks[1] = 0x0000ff00; masks[2] = 0x000000ff; masks[3] = 0xff000000; }
        } else return false;
        if (!masks[0] && !masks[1] && !masks[2]) return false;
        // Windows' synthesized CF_DIBV5 repeats the masks after a V4/V5 header
        // (CF_DIB layout); detect the copy, or a payload exactly that long.
        const u64 pixels = (u64{w} * bits + 31) / 32 * 4 * h;
        if (header > 40 && compression != rgb && offset + 12 <= dib.count) {
            const bool repeated = byte_load_le32(dib.data + offset) == masks[0] && byte_load_le32(dib.data + offset + 4) == masks[1] &&
                                  byte_load_le32(dib.data + offset + 8) == masks[2];
            if ((repeated && offset + 12 + pixels <= dib.count) || (dib.count >= pixels && dib.count - pixels == offset + 12 + u64{used} * 4))
                offset += 12;
        }
    } else if (bits == 24 || bits == 1 || bits == 4 || bits == 8) {
        if (compression != rgb) return false;
    } else return false;
    if (bits > 8 && used > 256) return false;
    const u32 palette = bits <= 8 ? (used && used <= (1u << bits) ? used : 1u << bits) : used;
    out->palette = dib.data + offset;
    out->palette_count = bits <= 8 ? palette : 0;
    offset += u64{palette} * 4;
    const u64 stride = (u64{w} * bits + 31) / 32 * 4;
    if (offset > dib.count || stride * h > dib.count - offset) return false;
    out->pixels = dib.data + offset;
    out->width = w; out->height = h; out->stride = static_cast<u32>(stride); out->bits = bits;
    out->top_down = height < 0;
    out->red = channel(masks[0]); out->green = channel(masks[1]); out->blue = channel(masks[2]); out->alpha = channel(masks[3]);
    out->plain = bits == 32 && masks[0] == 0x00ff0000 && masks[1] == 0x0000ff00 && masks[2] == 0x000000ff &&
                 (masks[3] == 0xff000000 || !masks[3]);
    return true;
}
// Row y as `channels` bytes for each pixel: red, green, blue and, with four channels, alpha. The two common layouts
// have loops without a branch for each pixel.
void dib_row(const Dib& dib, u32 y, u8* output, u32 channels) noexcept {
    const u8* row = dib.pixels + u64{dib.top_down ? y : dib.height - 1 - y} * dib.stride;
    const bool with_alpha = channels == 4;
    if (dib.plain) {
        // Four bytes go out for each pixel. With three channels the next pixel writes over the fourth byte, and the
        // output has room for the fourth byte of the last pixel.
        const u32 opaque = dib.use_alpha ? 0 : 0xff000000u;
        for (u32 x = 0; x < dib.width; ++x)
            byte_store_le32(output + u64{x} * channels, swap_red_blue(byte_load_le32(row + u64{x} * 4)) | opaque);
        return;
    }
    if (dib.bits == 24) {
        for (u32 x = 0; x < dib.width; ++x) {
            const u8* p = row + u64{x} * 3;
            u8* out = output + u64{x} * channels;
            out[0] = p[2]; out[1] = p[1]; out[2] = p[0];
            if (with_alpha) out[3] = 255;
        }
        return;
    }
    for (u32 x = 0; x < dib.width; ++x) {
        u8* out = output + u64{x} * channels;
        u8 alpha = 255;
        if (dib.bits <= 8) {
            const u32 bit = x * dib.bits;
            const u32 index = (row[bit >> 3] >> (8 - dib.bits - (bit & 7))) & ((1u << dib.bits) - 1u);
            const u8* entry = dib.palette + (index < dib.palette_count ? index : 0) * 4;
            out[0] = entry[2]; out[1] = entry[1]; out[2] = entry[0];
        } else {
            const u32 pixel = dib.bits == 16 ? byte_load_le16(row + x * 2) : byte_load_le32(row + u64{x} * 4);
            out[0] = extract(pixel, dib.red); out[1] = extract(pixel, dib.green); out[2] = extract(pixel, dib.blue);
            if (dib.use_alpha) alpha = extract(pixel, dib.alpha);
        }
        if (with_alpha) out[3] = alpha;
    }
}
// ---- PNG with filtered rows and a fixed-Huffman LZ77 stream ------------------
// The eight bits of a byte in the opposite order: Huffman codes go into the stream from their first bit.
constexpr u32 reverse8(u32 byte) noexcept {
    return static_cast<u32>(((byte * 0x80200802ull) & 0x0884422110ull) * 0x0101010101ull >> 32) & 0xff;
}
static_assert(reverse8(0x01) == 0x80 && reverse8(0x30) == 0x0c && reverse8(0xa5) == 0xa5 && reverse8(0xfe) == 0x7f);
struct Bits {
    u8* out; u64 capacity, cursor; u64 buffer; u32 count;
    __forceinline void put(u32 value, u32 n) noexcept {
        buffer |= u64{value} << count; count += n;
        while (count >= 8) { if (cursor < capacity) out[cursor] = static_cast<u8>(buffer); ++cursor; buffer >>= 8; count -= 8; }
    }
    // A code of at most 8 bits.
    __forceinline void code(u32 value, u32 n) noexcept { put(reverse8(value) >> (8 - n), n); }
    __forceinline void literal(u32 v) noexcept { if (v < 144) code(0x30 + v, 8); else put(reverse8((0x190 + v - 144) & 0xff) << 1 | 1, 9); }
    __forceinline void symbol(u32 s) noexcept { if (s < 280) code(s - 256, 7); else code(0xc0 + s - 280, 8); }
    void flush() noexcept { if (count) put(0, 8 - count); }
};
constexpr u16 kLengthBase[29]{3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr u8 kLengthExtra[29]{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr u16 kDistanceBase[30]{1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr u8 kDistanceExtra[30]{0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
constexpr u32 kWindow = 32768, kHashBits = 15, kChain = 12;
// The index of the last entry of kLengthBase that is not above a match length of 3 to 258.
constexpr u32 length_code(u32 length) noexcept {
    const u32 m = length - 3;
    if (m < 8) return m;
    if (length == 258) return 28;
    const u32 top = 31 - static_cast<u32>(__builtin_clz(m));
    return 4 * (top - 1) + ((m >> (top - 2)) & 3);
}
// The index of the last entry of kDistanceBase that is not above a distance of 1 to 32768.
constexpr u32 distance_code(u32 distance) noexcept {
    const u32 m = distance - 1;
    if (m < 4) return m;
    const u32 top = 31 - static_cast<u32>(__builtin_clz(m));
    return 2 * top + ((m >> (top - 1)) & 1);
}
static_assert([]() constexpr {
    for (u32 i = 0; i != 29; ++i)
        if (length_code(kLengthBase[i]) != i || (i != 28 && length_code(kLengthBase[i + 1] - 1u) != i)) return false;
    for (u32 i = 0; i != 30; ++i)
        if (distance_code(kDistanceBase[i]) != i || distance_code(i == 29 ? 32768u : kDistanceBase[i + 1] - 1u) != i) return false;
    return true;
}(), "the code formulas agree with the base tables");
// The count of equal bytes at a and b, at most limit.
__forceinline u32 match_length(const u8* a, const u8* b, u32 limit) noexcept {
    u32 length = 0;
    for (; length + 8 <= limit; length += 8)
        if (const u64 difference = byte_load_le64(a + length) ^ byte_load_le64(b + length))
            return length + lowest_set_bit(difference) / 8;
    while (length < limit && a[length] == b[length]) ++length;
    return length;
}
// Returns bytes written; a result above capacity means it did not fit. `data` has 1 readable byte after `count`.
u64 deflate_fixed(const u8* data, u64 count, u8* out, u64 capacity, u32* head, u32* previous) noexcept {
    Bits bits{out, capacity, 0, 0, 0};
    bits.put(1, 1); bits.put(1, 2);
    clear_bytes(head, static_cast<u32>(sizeof(u32) << kHashBits));
    // The three bytes at `at` as one number, first byte highest.
    const auto hash = [&](u64 at) __attribute__((always_inline)) noexcept {
        return (byte_swap(byte_load_le32(data + at)) >> 8) * 2654435761u >> (32 - kHashBits);
    };
    const auto insert = [&](u64 at) __attribute__((always_inline)) noexcept {
        if (at + 3 > count) return;
        const u32 h = hash(at);
        previous[at & (kWindow - 1)] = head[h];
        head[h] = static_cast<u32>(at + 1);
    };
    u64 position = 0;
    while (position < count) {
        u32 best = 0, distance = 0;
        if (position + 3 <= count) {
            const u32 limit = static_cast<u32>(minimum<u64>(258, count - position));
            u32 candidate = head[hash(position)], chain = kChain;
            while (candidate && chain--) {
                const u64 at = candidate - 1;
                if (position - at > kWindow) break;
                // A longer match also has the byte after the best length: most candidates fail this one compare.
                if (data[at + best] == data[position + best]) {
                    const u32 length = match_length(data + at, data + position, limit);
                    if (length > best) { best = length; distance = static_cast<u32>(position - at); if (length == limit) break; }
                }
                candidate = previous[at & (kWindow - 1)];
            }
        }
        if (best >= 3) {
            const u32 l = length_code(best);
            bits.symbol(257 + l); bits.put(best - kLengthBase[l], kLengthExtra[l]);
            const u32 d = distance_code(distance);
            bits.code(d, 5); bits.put(distance - kDistanceBase[d], kDistanceExtra[d]);
            for (u32 i = 0; i < best; ++i) insert(position + i);
            position += best;
        } else {
            bits.literal(data[position]); insert(position); ++position;
        }
        if (bits.cursor > capacity) return bits.cursor;
    }
    bits.symbol(256); bits.flush();
    return bits.cursor;
}
MAICORD_HASWELL __forceinline __m256i byte_distance(__m256i x, __m256i y) noexcept {
    return _mm256_or_si256(_mm256_subs_epu8(x, y), _mm256_subs_epu8(y, x));
}
// All ones in each byte where x is not above y, without a sign.
MAICORD_HASWELL __forceinline __m256i not_above(__m256i x, __m256i y) noexcept {
    return _mm256_cmpeq_epi8(_mm256_min_epu8(x, y), x);
}
MAICORD_HASWELL __forceinline __m256i choose(__m256i mask, __m256i yes, __m256i no) noexcept {
    return _mm256_xor_si256(no, _mm256_and_si256(_mm256_xor_si256(yes, no), mask));
}
// Chooses the filter with the smallest sum of signed residuals for the row and writes its residuals. `row` and
// `above` each have 32 zero bytes before them: the pixel to the left of the first one. `candidates` holds five rows.
MAICORD_HASWELL void filter_row(const u8* row, const u8* above, u32 bytes, u32 bpp, u8* candidates, u8* out) noexcept {
    const __m256i zero = _mm256_setzero_si256(), one = _mm256_set1_epi8(1);
    __m256i sums[5]{zero, zero, zero, zero, zero};
    u32 x = 0;
    for (; x + 32 <= bytes; x += 32) {
        const __m256i current = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(row + x));
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(row + x - bpp));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(above + x));
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i_u*>(above + x - bpp));
        const __m256i average = _mm256_sub_epi8(_mm256_avg_epu8(a, b), _mm256_and_si256(_mm256_xor_si256(a, b), one));
        // Paeth: pa = |b - c|, pb = |a - c|, pc = |(a - c) + (b - c)|. With two steps of one sign pc is their sum
        // (255 when it is more: no pa or pb is above that). Else pc is the distance between pa and pb.
        const __m256i pa = byte_distance(b, c), pb = byte_distance(a, c);
        const __m256i opposite = _mm256_xor_si256(not_above(c, a), not_above(c, b));
        const __m256i pc = choose(opposite, byte_distance(pa, pb), _mm256_adds_epu8(pa, pb));
        const __m256i paeth =
            choose(_mm256_and_si256(not_above(pa, pb), not_above(pa, pc)), a, choose(not_above(pb, pc), b, c));
        const __m256i residuals[5]{current, _mm256_sub_epi8(current, a), _mm256_sub_epi8(current, b),
                                   _mm256_sub_epi8(current, average), _mm256_sub_epi8(current, paeth)};
        for (u32 f = 0; f != 5; ++f) {
            _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(candidates + u64{f} * bytes + x), residuals[f]);
            sums[f] = _mm256_add_epi64(sums[f], _mm256_sad_epu8(_mm256_abs_epi8(residuals[f]), zero));
        }
    }
    u64 cost[5];
    for (u32 f = 0; f != 5; ++f) {
        u64 lanes[4];
        _mm256_storeu_si256(reinterpret_cast<__m256i_u*>(lanes), sums[f]);
        cost[f] = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    }
    for (; x < bytes; ++x) {
        const u8 a = (row + x)[-static_cast<i32>(bpp)], b = above[x], c = (above + x)[-static_cast<i32>(bpp)];
        const i32 p = i32{a} + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
        const u8 predicted[5]{0, a, b, static_cast<u8>((u32{a} + b) >> 1), pa <= pb && pa <= pc ? a : pb <= pc ? b : c};
        for (u32 f = 0; f != 5; ++f) {
            const u8 residual = static_cast<u8>(row[x] - predicted[f]);
            candidates[u64{f} * bytes + x] = residual;
            cost[f] += residual < 128 ? residual : 256u - residual;
        }
    }
    u8 best = 0;
    for (u8 f = 1; f < 5; ++f) if (cost[f] < cost[best]) best = f;
    out[0] = best;
    copy_bytes(out + 1, candidates + u64{best} * bytes, bytes);
}
DWORD encode_dib_png(const Dib& source, u64 limit, ClipboardPng* output) noexcept {
    Dib dib = source;
    const u32 w = dib.width, h = dib.height;
    // Two rows with 32 zero bytes before each (see filter_row), then the five filtered candidates of one row.
    const u64 row_room = u64{w} * 4 + 64;
    u8* rows = commit_pages<u8>(row_room * 2 + u64{w} * 20);
    if (!rows) return ERROR_NOT_ENOUGH_MEMORY;
    u8* packed[2]{rows + 32, rows + 32 + row_room};
    // Alpha only counts when present and not uniformly zero (an unused byte). It makes a fourth channel when one
    // pixel is not opaque.
    bool translucent = false;
    if (dib.alpha.mask) {
        dib.use_alpha = true;
        u8 some = 0, every = 255;
        for (u32 y = 0; y < h; ++y) {
            const u8* alpha = packed[0] + 3;
            if (dib.plain) alpha = dib.pixels + u64{y} * dib.stride + 3;
            else dib_row(dib, y, packed[0], 4);
            for (u32 x = 0; x < w; ++x) { some |= alpha[u64{x} * 4]; every &= alpha[u64{x} * 4]; }
        }
        dib.use_alpha = some != 0;
        translucent = some && every != 255;
    }
    const u32 channels = translucent ? 4 : 3, row_bytes = w * channels;
    const u64 raw = u64{h} * (1 + row_bytes);
    const u64 stored = zlib_bytes(raw) - 6;
    const u64 png_capacity = 8 + 25 + 12 + 6 + stored + 12;
    const u64 tables = (sizeof(u32) << kHashBits) + sizeof(u32) * kWindow;
    u8* filtered = commit_pages<u8>(tables + raw + 1); // deflate_fixed reads one byte after the data
    u8* png = filtered ? commit_pages<u8>(png_capacity) : nullptr;
    if (!png) { unreserve(filtered); unreserve(rows); return ERROR_NOT_ENOUGH_MEMORY; }
    auto* head = reinterpret_cast<u32*>(filtered);
    u8* data = filtered + tables;
    // The row above the first one is the second buffer, which is still zero.
    for (u32 y = 0; y < h; ++y) {
        u8* row = packed[y & 1];
        dib_row(dib, y, row, channels);
        filter_row(row, packed[(y + 1) & 1], row_bytes, channels, rows + row_room * 2, data + u64{y} * (1 + row_bytes));
    }
    unreserve(rows);
    u8* idat = write_header(png, w, h, channels == 4 ? 6 : 2);
    u8* z = idat + 8;
    z[0] = 0x78; z[1] = 0x01;
    u64 body = deflate_fixed(data, raw, z + 2, stored, head, head + (u64{1} << kHashBits));
    if (body > stored) {
        // Incompressible: stored blocks bound the size at raw + 5 per 64 KiB.
        u64 cursor = 0, emitted = 0;
        do {
            const u32 block = static_cast<u32>(minimum<u64>(kStoredBlock, raw - emitted));
            u8* b = z + 2 + cursor;
            store_header(b, block, emitted + block == raw);
            copy_bytes(b + 5, data + emitted, block);
            cursor += 5 + u64{block}; emitted += block;
        } while (emitted < raw);
        body = cursor;
    }
    u32 a = 1, b = 0;
    adler32_update(&a, &b, data, raw);
    byte_store_be32(z + 2 + body, (b << 16) | a);
    const u64 zlib = 2 + body + 4;
    unreserve(filtered);
    u8* iend = chunk(idat, "IDAT", z, static_cast<u32>(zlib));
    u8* end = chunk(iend, "IEND", nullptr, 0);
    const u64 total = static_cast<u64>(end - png);
    if (total > limit) { unreserve(png); output->bytes = total; return ERROR_FILE_TOO_LARGE; }
    *output = {png, total};
    return ERROR_SUCCESS;
}
} // namespace
u32 clipboard_paste_plan(ClipboardPasteFormats formats, ClipboardPasteKind (&plan)[kClipboardPastePlanCapacity]) noexcept {
    u32 count = 0;
    if (formats.png) plan[count++] = ClipboardPasteKind::png;
    if (formats.dibv5) plan[count++] = ClipboardPasteKind::dibv5;
    if (formats.dib) plan[count++] = ClipboardPasteKind::dib;
    if (formats.files) plan[count++] = ClipboardPasteKind::files;
    if (formats.virtual_files) plan[count++] = ClipboardPasteKind::virtual_files;
    return count;
}
u64 clipboard_image_png_length(ConstSlice<u8> png) noexcept {
    if (!png.data || png.count < 8 + 25 + 12 || !bytes_equal(png.data, kPngSignature, 8) ||
        byte_load_be32(png.data + 8) != 13 || !bytes_equal(png.data + 12, "IHDR", 4))
        return 0;
    u64 cursor = 8;
    while (cursor + 12 <= png.count) {
        const u32 length = byte_load_be32(png.data + cursor);
        if (length > 0x7fffffffu || cursor + 12 + length > png.count) return 0;
        const bool end = bytes_equal(png.data + cursor + 4, "IEND", 4);
        cursor += 12 + u64{length};
        if (end) return cursor;
    }
    return 0;
}
DWORD clipboard_paste_png(ClipboardPasteKind kind, ConstSlice<u8> payload, u64 limit, ClipboardPng* output) noexcept {
    if (!output || !payload.data || !limit) return ERROR_INVALID_PARAMETER;
    reset_value(output);
    if (kind == ClipboardPasteKind::png) {
        const u64 bytes = clipboard_image_png_length(payload);
        if (!bytes) return ERROR_INVALID_DATA;
        if (bytes > limit) { output->bytes = bytes; return ERROR_FILE_TOO_LARGE; }
        u8* copy = commit_pages<u8>(bytes);
        if (!copy) return ERROR_NOT_ENOUGH_MEMORY;
        copy_bytes(copy, payload.data, static_cast<u32>(bytes));
        *output = {copy, bytes};
        return ERROR_SUCCESS;
    }
    if (kind != ClipboardPasteKind::dib && kind != ClipboardPasteKind::dibv5) return ERROR_INVALID_PARAMETER;
    Dib dib{};
    if (!dib_layout(payload, &dib)) return ERROR_INVALID_DATA;
    return encode_dib_png(dib, limit, output);
}
void clipboard_png_release(ClipboardPng* png) noexcept {
    if (png) { unreserve(png->data); reset_value(png); }
}
#endif
} // namespace maicord
