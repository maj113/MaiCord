#include "maicord/platform/clipboard_image.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/gdi.hpp"

extern "C" __declspec(dllimport) unsigned long __stdcall RtlComputeCrc32(
    unsigned long initial, const void* bytes, unsigned long count);

namespace maicord {
namespace {
constexpr u32 kStoredBlock = 65535;
bool extent_valid(u32 width, u32 height) noexcept {
    return width && height && width <= kClipboardImageMaximumExtent && height <= kClipboardImageMaximumExtent;
}
u32 crc32_update(u32 crc, const u8* data, u64 count) noexcept {
    // Keep the published CRC seed across ULONG-sized chunks and preserve the
    // seed for an empty range. PNG uses the same IEEE CRC as png_common.cpp.
    while (count) {
        const u32 bytes = static_cast<u32>(minimum<u64>(count, 0xffffffffull));
        crc = static_cast<u32>(RtlComputeCrc32(crc, data, bytes));
        data += bytes;
        count -= bytes;
    }
    return crc;
}
void put_u32_be(u8* out, u32 value) noexcept {
    out[0] = static_cast<u8>(value >> 24);
    out[1] = static_cast<u8>(value >> 16);
    out[2] = static_cast<u8>(value >> 8);
    out[3] = static_cast<u8>(value);
}
u64 raw_bytes(u32 width, u32 height) noexcept {
    return u64{height} * (1 + u64{width} * 4);
}
u64 zlib_bytes(u64 raw) noexcept {
    const u64 blocks = raw ? (raw + kStoredBlock - 1) / kStoredBlock : 1;
    return 2 + raw + 5 * blocks + 4;
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
    u8* out = output.data;
    constexpr u8 signature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    copy_bytes(out, signature, 8);
    u8* ihdr = out + 8;
    put_u32_be(ihdr, 13);
    copy_bytes(ihdr + 4, "IHDR", 4);
    put_u32_be(ihdr + 8, width);
    put_u32_be(ihdr + 12, height);
    ihdr[16] = 8; // bit depth
    ihdr[17] = 6; // truecolor with alpha
    ihdr[18] = ihdr[19] = ihdr[20] = 0;
    put_u32_be(ihdr + 21, crc32_update(0, ihdr + 4, 17));
    const u64 raw = raw_bytes(width, height), zlib = zlib_bytes(raw);
    u8* idat = ihdr + 25;
    put_u32_be(idat, static_cast<u32>(zlib));
    copy_bytes(idat + 4, "IDAT", 4);
    u8* z = idat + 8;
    z[0] = 0x78; // deflate, 32 KiB window
    z[1] = 0x01; // no preset dictionary, fastest; 0x7801 % 31 == 0
    u64 cursor = 2, emitted = 0;
    u32 adler_a = 1, adler_b = 0;
    u32 row = 0, column = 0; // column 0 is the filter byte, then RGBA bytes
    const u32 row_bytes = 1 + width * 4;
    do {
        const u32 block = static_cast<u32>(minimum<u64>(kStoredBlock, raw - emitted));
        z[cursor++] = emitted + block == raw ? 1 : 0;
        z[cursor++] = static_cast<u8>(block);
        z[cursor++] = static_cast<u8>(block >> 8);
        z[cursor++] = static_cast<u8>(~block);
        z[cursor++] = static_cast<u8>(~block >> 8);
        for (u32 i = 0; i < block;) {
            // With reduced initial sums and bytes <= 255, 5552 additions keep
            // both Adler sums within u32. Include each row's zero filter byte.
            const u32 end = i + minimum<u32>(5552, block - i);
            for (; i < end; ++i) {
                const u8 value = column ? rgba[u64{row} * stride + column - 1] : 0;
                z[cursor++] = value;
                adler_a += value;
                adler_b += adler_a;
                if (++column == row_bytes) {
                    column = 0;
                    ++row;
                }
            }
            adler_a %= 65521;
            adler_b %= 65521;
        }
        emitted += block;
    } while (emitted < raw);
    put_u32_be(z + cursor, (adler_b << 16) | adler_a);
    cursor += 4;
    put_u32_be(z + cursor, crc32_update(0, idat + 4, 4 + zlib));
    u8* iend = z + cursor + 4;
    put_u32_be(iend, 0);
    copy_bytes(iend + 4, "IEND", 4);
    put_u32_be(iend + 8, crc32_update(0, iend + 4, 4));
    return total;
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
        for (u32 x = 0; x < width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return total;
}

namespace {
// ---- Paste conversion -------------------------------------------------------
u32 load_u32_le(const u8* p) noexcept { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
u32 load_u32_be(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | u32{p[3]}; }
u8* reserve(u64 bytes) noexcept {
    return bytes && bytes <= (u64{1} << 40)
               ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
               : nullptr;
}
void unreserve(void* memory) noexcept { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
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
};
bool dib_layout(ConstSlice<u8> dib, Dib* out) noexcept {
    *out = {};
    if (!dib.data || dib.count < 40) return false;
    const u32 header = load_u32_le(dib.data);
    const i32 width = static_cast<i32>(load_u32_le(dib.data + 4)), height = static_cast<i32>(load_u32_le(dib.data + 8));
    const u32 planes = u32{dib.data[12]} | u32{dib.data[13]} << 8, bits = u32{dib.data[14]} | u32{dib.data[15]} << 8;
    const u32 compression = load_u32_le(dib.data + 16), used = load_u32_le(dib.data + 32);
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
                for (u32 i = 0; i < 4 && 40 + i * 4 + 4 <= header; ++i) masks[i] = load_u32_le(dib.data + 40 + i * 4);
            } else {
                // A BITMAPINFOHEADER carries its masks after the header.
                if (offset + count * 4 > dib.count) return false;
                for (u32 i = 0; i < count; ++i) masks[i] = load_u32_le(dib.data + offset + i * 4);
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
            const bool repeated = load_u32_le(dib.data + offset) == masks[0] && load_u32_le(dib.data + offset + 4) == masks[1] &&
                                  load_u32_le(dib.data + offset + 8) == masks[2];
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
    return true;
}
void dib_row(const Dib& dib, u32 y, u8* rgba) noexcept {
    const u8* row = dib.pixels + u64{dib.top_down ? y : dib.height - 1 - y} * dib.stride;
    for (u32 x = 0; x < dib.width; ++x) {
        u8* out = rgba + u64{x} * 4;
        if (dib.bits <= 8) {
            const u32 bit = x * dib.bits;
            const u32 index = (row[bit >> 3] >> (8 - dib.bits - (bit & 7))) & ((1u << dib.bits) - 1u);
            const u8* entry = dib.palette + (index < dib.palette_count ? index : 0) * 4;
            out[0] = entry[2]; out[1] = entry[1]; out[2] = entry[0]; out[3] = 255;
        } else if (dib.bits == 24) {
            const u8* p = row + u64{x} * 3;
            out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
        } else {
            const u32 pixel = dib.bits == 16 ? u32{row[x * 2]} | u32{row[x * 2 + 1]} << 8 : load_u32_le(row + u64{x} * 4);
            out[0] = extract(pixel, dib.red); out[1] = extract(pixel, dib.green); out[2] = extract(pixel, dib.blue);
            out[3] = dib.use_alpha ? extract(pixel, dib.alpha) : 255;
        }
    }
}
// ---- PNG with filtered rows and a fixed-Huffman LZ77 stream ------------------
struct Bits {
    u8* out; u64 capacity, cursor; u64 buffer; u32 count;
    void put(u32 value, u32 n) noexcept {
        buffer |= u64{value} << count; count += n;
        while (count >= 8) { if (cursor < capacity) out[cursor] = static_cast<u8>(buffer); ++cursor; buffer >>= 8; count -= 8; }
    }
    void code(u32 value, u32 n) noexcept { u32 r = 0; for (u32 i = 0; i < n; ++i) r |= ((value >> i) & 1) << (n - 1 - i); put(r, n); }
    void literal(u32 v) noexcept { if (v < 144) code(0x30 + v, 8); else code(0x190 + v - 144, 9); }
    void symbol(u32 s) noexcept { if (s < 280) code(s - 256, 7); else code(0xc0 + s - 280, 8); }
    void flush() noexcept { if (count) put(0, 8 - count); }
};
constexpr u16 kLengthBase[29]{3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr u8 kLengthExtra[29]{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr u16 kDistanceBase[30]{1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr u8 kDistanceExtra[30]{0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
constexpr u32 kWindow = 32768, kHashBits = 15, kChain = 12;
// Returns bytes written; a result above capacity means it did not fit.
u64 deflate_fixed(const u8* data, u64 count, u8* out, u64 capacity, u32* head, u32* previous) noexcept {
    Bits bits{out, capacity, 0, 0, 0};
    bits.put(1, 1); bits.put(1, 2);
    clear_bytes(head, static_cast<u32>(sizeof(u32) << kHashBits));
    const auto hash = [&](u64 at) noexcept {
        return ((u32{data[at]} << 16) | (u32{data[at + 1]} << 8) | data[at + 2]) * 2654435761u >> (32 - kHashBits);
    };
    const auto insert = [&](u64 at) noexcept {
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
                u32 length = 0;
                while (length < limit && data[at + length] == data[position + length]) ++length;
                if (length > best) { best = length; distance = static_cast<u32>(position - at); if (length == limit) break; }
                candidate = previous[at & (kWindow - 1)];
            }
        }
        if (best >= 3) {
            u32 l = 28; while (kLengthBase[l] > best) --l;
            bits.symbol(257 + l); bits.put(best - kLengthBase[l], kLengthExtra[l]);
            u32 d = 29; while (kDistanceBase[d] > distance) --d;
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
u8 paeth_predict(u8 a, u8 b, u8 c) noexcept {
    const i32 p = i32{a} + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
u8 predict(u8 filter, const u8* row, const u8* above, u32 x, u32 bpp) noexcept {
    const u8 a = x >= bpp ? row[x - bpp] : 0, b = above ? above[x] : 0, c = above && x >= bpp ? above[x - bpp] : 0;
    return filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b : filter == 3 ? static_cast<u8>((u32{a} + b) >> 1) : paeth_predict(a, b, c);
}
// Chooses the filter with the smallest sum of signed residuals per row.
void filter_row(const u8* row, const u8* above, u32 bytes, u32 bpp, u8* out) noexcept {
    u64 best_cost = ~0ull; u8 best = 0;
    for (u8 f = 0; f < 5; ++f) {
        u64 cost = 0;
        for (u32 x = 0; x < bytes; ++x) {
            const u8 residual = static_cast<u8>(row[x] - predict(f, row, above, x, bpp));
            cost += residual < 128 ? residual : 256u - residual;
        }
        if (cost < best_cost) { best_cost = cost; best = f; }
    }
    out[0] = best;
    for (u32 x = 0; x < bytes; ++x) out[1 + x] = static_cast<u8>(row[x] - predict(best, row, above, x, bpp));
}
u8* chunk(u8* out, const char* type, const u8* data, u32 bytes) noexcept {
    put_u32_be(out, bytes); copy_bytes(out + 4, type, 4);
    if (bytes && data != out + 8) copy_bytes(out + 8, data, bytes);
    put_u32_be(out + 8 + bytes, crc32_update(0, out + 4, 4 + u64{bytes}));
    return out + 12 + bytes;
}
DWORD encode_dib_png(const Dib& source, u64 limit, ClipboardPng* output) noexcept {
    Dib dib = source;
    const u32 w = dib.width, h = dib.height;
    u8* rows = reserve(u64{w} * 8);
    if (!rows) return ERROR_NOT_ENOUGH_MEMORY;
    // Alpha only counts when present and not uniformly zero (an unused byte).
    bool translucent = false;
    if (dib.alpha.mask) {
        dib.use_alpha = true;
        bool any = false;
        for (u32 y = 0; y < h && !any; ++y) {
            dib_row(dib, y, rows);
            for (u32 x = 0; x < w; ++x) if (rows[x * 4 + 3]) { any = true; break; }
        }
        dib.use_alpha = any;
        for (u32 y = 0; any && y < h && !translucent; ++y) {
            dib_row(dib, y, rows);
            for (u32 x = 0; x < w; ++x) if (rows[x * 4 + 3] != 255) { translucent = true; break; }
        }
    }
    const u32 channels = translucent ? 4 : 3, row_bytes = w * channels;
    const u64 raw = u64{h} * (1 + row_bytes);
    const u64 stored = zlib_bytes(raw) - 6;
    const u64 png_capacity = 8 + 25 + 12 + 6 + stored + 12;
    const u64 tables = (sizeof(u32) << kHashBits) + sizeof(u32) * kWindow;
    u8* filtered = reserve(tables + raw);
    u8* png = filtered ? reserve(png_capacity) : nullptr;
    if (!png) { unreserve(filtered); unreserve(rows); return ERROR_NOT_ENOUGH_MEMORY; }
    auto* head = reinterpret_cast<u32*>(filtered);
    u8* data = filtered + tables;
    u8* packed[2]{rows, rows + u64{w} * 4};
    for (u32 y = 0; y < h; ++y) {
        u8* row = packed[y & 1];
        dib_row(dib, y, row);
        if (channels == 3)
            for (u32 x = 0; x < w; ++x) { row[x * 3] = row[x * 4]; row[x * 3 + 1] = row[x * 4 + 1]; row[x * 3 + 2] = row[x * 4 + 2]; }
        filter_row(row, y ? packed[(y + 1) & 1] : nullptr, row_bytes, channels, data + u64{y} * (1 + row_bytes));
    }
    unreserve(rows);
    constexpr u8 signature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    copy_bytes(png, signature, 8);
    u8 header[13]{};
    put_u32_be(header, w); put_u32_be(header + 4, h);
    header[8] = 8; header[9] = channels == 4 ? 6 : 2;
    u8* idat = chunk(png + 8, "IHDR", header, 13);
    u8* z = idat + 8;
    z[0] = 0x78; z[1] = 0x01;
    u64 body = deflate_fixed(data, raw, z + 2, stored, head, head + (u64{1} << kHashBits));
    if (body > stored) {
        // Incompressible: stored blocks bound the size at raw + 5 per 64 KiB.
        u64 cursor = 0, emitted = 0;
        do {
            const u32 block = static_cast<u32>(minimum<u64>(kStoredBlock, raw - emitted));
            u8* b = z + 2 + cursor;
            b[0] = emitted + block == raw ? 1 : 0;
            b[1] = static_cast<u8>(block); b[2] = static_cast<u8>(block >> 8);
            b[3] = static_cast<u8>(~block); b[4] = static_cast<u8>(~block >> 8);
            copy_bytes(b + 5, data + emitted, block);
            cursor += 5 + u64{block}; emitted += block;
        } while (emitted < raw);
        body = cursor;
    }
    u32 a = 1, b = 0;
    for (u64 i = 0; i < raw;) {
        const u64 end = minimum<u64>(raw, i + 5552);
        for (; i < end; ++i) { a += data[i]; b += a; }
        a %= 65521; b %= 65521;
    }
    put_u32_be(z + 2 + body, (b << 16) | a);
    const u64 zlib = 2 + body + 4;
    unreserve(filtered);
    u8* iend = chunk(idat, "IDAT", z, static_cast<u32>(zlib));
    u8* end = chunk(iend, "IEND", nullptr, 0);
    const u64 total = static_cast<u64>(end - png);
    if (total > limit) { unreserve(png); return ERROR_FILE_TOO_LARGE; }
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
    constexpr u8 signature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!png.data || png.count < 8 + 25 + 12 || !bytes_equal(png.data, signature, 8) ||
        load_u32_be(png.data + 8) != 13 || !bytes_equal(png.data + 12, "IHDR", 4))
        return 0;
    u64 cursor = 8;
    while (cursor + 12 <= png.count) {
        const u32 length = load_u32_be(png.data + cursor);
        if (length > 0x7fffffffu || cursor + 12 + length > png.count) return 0;
        const bool end = bytes_equal(png.data + cursor + 4, "IEND", 4);
        cursor += 12 + u64{length};
        if (end) return cursor;
    }
    return 0;
}
DWORD clipboard_paste_png(ClipboardPasteKind kind, ConstSlice<u8> payload, u64 limit, ClipboardPng* output) noexcept {
    if (!output || !payload.data || !limit) return ERROR_INVALID_PARAMETER;
    *output = {};
    if (kind == ClipboardPasteKind::png) {
        const u64 bytes = clipboard_image_png_length(payload);
        if (!bytes) return ERROR_INVALID_DATA;
        if (bytes > limit) return ERROR_FILE_TOO_LARGE;
        u8* copy = reserve(bytes);
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
    if (png) { unreserve(png->data); *png = {}; }
}
} // namespace maicord
