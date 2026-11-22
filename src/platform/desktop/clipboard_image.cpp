#include "maicord/platform/desktop/clipboard_image.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "maicord/core/adler32.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/media/image/png_encode.hpp"
#if defined(_WIN32)
#include "maicord/sys/gdi.hpp"
#endif

namespace maicord {
namespace {
constexpr u32 kStoredBlock = 65535;
bool extent_valid(u32 width, u32 height) noexcept {
    return width && height && width <= kClipboardImageMaximumExtent && height <= kClipboardImageMaximumExtent;
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
} // namespace
u64 clipboard_image_png_bytes(u32 width, u32 height) noexcept {
    if (!extent_valid(width, height))
        return 0;
    // Stored blocks of 65535 bytes fill the zlib_compress bound exactly.
    return kPngFrameBytes + zlib_compress_bound(raw_bytes(width, height));
}
u64 clipboard_image_encode_png(const u8* rgba, u32 width, u32 height, u32 stride, Slice<u8> output) noexcept {
    const u64 total = clipboard_image_png_bytes(width, height);
    if (!rgba || !total || stride < width * 4 || !output.data || output.count < total)
        return 0;
    u8* idat = png_write_header(output.data, width, height, 6); // truecolor with alpha
    const u64 raw = raw_bytes(width, height), zlib = zlib_compress_bound(raw);
    u8* z = idat + 8;
    z[0] = 0x78; // deflate, 32 KiB window
    z[1] = 0x01; // no preset dictionary, fastest; 0x7801 % 31 == 0
    // The raw stream is a zero filter byte and the RGBA bytes for each row. It goes into stored blocks of 65535 bytes.
    u64 cursor = 2, left = raw;
    u32 room = 0, adler = 1;
    const u8 no_filter = 0;
    for (u32 row = 0; row < height; ++row) {
        const ConstSlice<u8> parts[2]{{&no_filter, 1}, {rgba + u64{row} * stride, width * 4}};
        for (const ConstSlice<u8>& part : parts) {
            adler = adler32_update(adler, part);
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
    byte_store_be32(z + cursor, adler);
    png_write_end(idat, static_cast<u32>(zlib));
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
constexpr u8 kPngSignature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
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
DWORD encode_dib_png(const Dib& source, u64 limit, ClipboardPng* output) noexcept {
    Dib dib = source;
    const u32 w = dib.width, h = dib.height;
    // Two rows with 32 zero bytes before each (see png_filter_row), then the five filtered candidates of one row.
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
    const u64 png_capacity = kPngFrameBytes + zlib_compress_bound(raw);
    u8* filtered = commit_pages<u8>(kZlibCompressWorkspaceBytes + raw);
    u8* png = filtered ? commit_pages<u8>(png_capacity) : nullptr;
    if (!png) { unreserve(filtered); unreserve(rows); return ERROR_NOT_ENOUGH_MEMORY; }
    u8* data = filtered + kZlibCompressWorkspaceBytes;
    // The row above the first one is the second buffer, which is still zero.
    for (u32 y = 0; y < h; ++y) {
        u8* row = packed[y & 1];
        dib_row(dib, y, row, channels);
        png_filter_row(row, packed[(y + 1) & 1], row_bytes, channels, rows + row_room * 2, data + u64{y} * (1 + row_bytes));
    }
    unreserve(rows);
    // The capacity holds the stored form, so the encoder cannot run out of room. The raw stream of an image of
    // kClipboardPasteMaximumPixels pixels is under 2^29 bytes.
    const u64 total = png_encode_filtered({data, static_cast<u32>(raw)}, w, h, channels == 4 ? 6 : 2,
                                          {filtered, kZlibCompressWorkspaceBytes}, {png, static_cast<u32>(png_capacity)});
    unreserve(filtered);
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
