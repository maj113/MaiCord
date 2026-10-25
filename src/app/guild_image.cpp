#include "maicord/app/guild_image.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/png.hpp"
#include "../media/png_common.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

struct Shape {
    u32 width, height;
    bool animated, ok;
};

bool starts(ConstSlice<u8> s, u32 at, const char* text, u32 bytes) noexcept {
    return s.count >= at + bytes && bytes_equal(s.data + at, text, bytes);
}

// IHDR dimensions; an acTL chunk before the first IDAT marks APNG.
Shape probe_png(ConstSlice<u8> s, bool* apng) noexcept {
    *apng = false;
    if (s.count < 45 || byte_load_be32(s.data + 8) != 13 || !starts(s, 12, "IHDR", 4))
        return {};
    Shape shape{byte_load_be32(s.data + 16), byte_load_be32(s.data + 20), false, true};
    u64 at = 8;
    while (at + 12 <= s.count) {
        const u32 length = byte_load_be32(s.data + at);
        const u8* type = s.data + at + 4;
        if (bytes_equal(type, "acTL", 4) && length >= 8 && at + 16 <= s.count) {
            *apng = true;
            shape.animated = byte_load_be32(s.data + at + 8) > 1;
        }
        if (bytes_equal(type, "IDAT", 4) || bytes_equal(type, "IEND", 4))
            break;
        at += 12ull + length;
    }
    return shape.width && shape.height ? shape : Shape{};
}

// Logical screen size; more than one image descriptor is an animation.
Shape probe_gif(ConstSlice<u8> s) noexcept {
    if (s.count < 14)
        return {};
    Shape shape{byte_load_le16(s.data + 6), byte_load_le16(s.data + 8), false, false};
    u64 at = 13;
    if (s[10] & 0x80)
        at += 3ull << ((s[10] & 7) + 1);
    const auto skip_blocks = [&]() noexcept {
        while (at < s.count) {
            const u8 length = s[static_cast<u32>(at)];
            ++at;
            if (!length)
                return true;
            at += length;
        }
        return false;
    };
    u32 frames = 0;
    while (at < s.count) {
        const u8 tag = s[static_cast<u32>(at)];
        if (tag == 0x3B)
            break;
        if (tag == 0x21) {
            at += 2;
            if (!skip_blocks())
                return {};
        } else if (tag == 0x2C) {
            if (at + 10 > s.count)
                return {};
            const u8 flags = s[static_cast<u32>(at + 9)];
            at += 10;
            if (flags & 0x80)
                at += 3ull << ((flags & 7) + 1);
            ++at; // LZW minimum code size
            if (!skip_blocks())
                return {};
            if (++frames > 1)
                break;
        } else
            return {};
    }
    shape.animated = frames > 1;
    shape.ok = frames && shape.width && shape.height;
    return shape;
}

// The first start-of-frame segment.
Shape probe_jpeg(ConstSlice<u8> s) noexcept {
    if (s.count < 4 || s[0] != 0xFF || s[1] != 0xD8)
        return {};
    u64 at = 2;
    while (at + 4 <= s.count) {
        if (s[static_cast<u32>(at)] != 0xFF)
            return {};
        u8 marker = s[static_cast<u32>(at + 1)];
        if (marker == 0xFF) {
            ++at;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            at += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA)
            return {};
        const u32 length = byte_load_be16(s.data + at + 2);
        if (length < 2)
            return {};
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (frame) {
            if (at + 9 > s.count || length < 7)
                return {};
            Shape shape{byte_load_be16(s.data + at + 7), byte_load_be16(s.data + at + 5), false, true};
            return shape.width && shape.height ? shape : Shape{};
        }
        at += 2ull + length;
    }
    return {};
}

Shape probe_webp(ConstSlice<u8> s) noexcept {
    if (s.count < 30)
        return {};
    const u8* data = s.data + 20;
    if (starts(s, 12, "VP8 ", 4)) {
        if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A)
            return {};
        return {byte_load_le16(data + 6) & 0x3FFFu, byte_load_le16(data + 8) & 0x3FFFu, false, true};
    }
    if (starts(s, 12, "VP8L", 4)) {
        if (data[0] != 0x2F)
            return {};
        const u32 bits = byte_load_le32(data + 1);
        return {(bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1, false, true};
    }
    if (starts(s, 12, "VP8X", 4))
        return {byte_load_le24(data + 4) + 1, byte_load_le24(data + 7) + 1, (data[0] & 0x02) != 0, true};
    return {};
}

// A Lottie animation is a JSON object with layers.
bool lottie(ConstSlice<u8> s) noexcept {
    u32 at = 0;
    while (at < s.count && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n'))
        ++at;
    if (at >= s.count || s[at] != '{' || !utf8_validate({reinterpret_cast<const char*>(s.data), s.count}).valid)
        return false;
    static constexpr char key[] = "\"layers\"";
    for (u32 i = at; i + sizeof(key) - 1 <= s.count; ++i)
        if (s[i] == '"' && bytes_equal(s.data + i, key, sizeof(key) - 1))
            return true;
    return false;
}

Shape probe(GuildImageKind kind, ConstSlice<u8> s) noexcept {
    bool apng = false;
    switch (kind) {
    case GuildImageKind::png:
    case GuildImageKind::apng: return probe_png(s, &apng);
    case GuildImageKind::gif: return probe_gif(s);
    case GuildImageKind::jpeg: return probe_jpeg(s);
    case GuildImageKind::webp: return probe_webp(s);
    default: return {};
    }
}

u8* allocate(u64 bytes) noexcept {
    return bytes && bytes <= ~0u ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes),
                                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                                 : nullptr;
}
void release(u8* data) noexcept {
    if (data)
        VirtualFree(data, 0, MEM_RELEASE);
}

// One straight-alpha RGBA8 pixel of decoded PNG scanlines.
void pixel(const PngImage& image, const u8* scanlines, u32 x, u32 y, u32 rgba[4]) noexcept {
    const u8* row = scanlines + static_cast<u64>(y) * image.row_stride + 1;
    const u32 depth = image.bit_depth;
    const auto packed = [&](u32 index) noexcept {
        const u32 bit = index * depth;
        return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    };
    // Sample `index` at full depth, and scaled to 8 bits.
    const auto raw = [&](u32 index) noexcept {
        return depth == 16 ? byte_load_be16(row + index * 2) : depth == 8 ? row[index] : packed(index);
    };
    const auto eight = [&](u32 index) noexcept {
        return depth == 16 ? static_cast<u32>(row[index * 2])
               : depth == 8 ? static_cast<u32>(row[index])
                            : packed(index) * 255 / ((1u << depth) - 1);
    };
    switch (image.color_type) {
    case 0:
        rgba[0] = rgba[1] = rgba[2] = eight(x);
        rgba[3] = raw(x) == image.transparent_gray ? 0 : 255;
        break;
    case 2:
        rgba[0] = eight(x * 3);
        rgba[1] = eight(x * 3 + 1);
        rgba[2] = eight(x * 3 + 2);
        rgba[3] = raw(x * 3) == image.transparent_red && raw(x * 3 + 1) == image.transparent_green &&
                          raw(x * 3 + 2) == image.transparent_blue
                      ? 0
                      : 255;
        break;
    case 3: {
        const u32 index = depth == 8 ? row[x] : packed(x);
        const u32 color = index < image.palette_count ? image.palette_rgba[index] : 0;
        rgba[0] = color & 0xFF;
        rgba[1] = color >> 8 & 0xFF;
        rgba[2] = color >> 16 & 0xFF;
        rgba[3] = color >> 24;
        break;
    }
    case 4:
        rgba[0] = rgba[1] = rgba[2] = eight(x * 2);
        rgba[3] = eight(x * 2 + 1);
        break;
    default:
        for (u32 c = 0; c < 4; ++c)
            rgba[c] = eight(x * 4 + c);
        break;
    }
}

// Area average of the crop rectangle into width x height, weighting colour
// by alpha so transparent edges do not darken.
void resample(const PngImage& image, const u8* scanlines, u32 cx, u32 cy, u32 cw, u32 ch, u32 width, u32 height,
              u8* output) noexcept {
    for (u32 dy = 0; dy < height; ++dy) {
        const u32 y0 = cy + static_cast<u32>(static_cast<u64>(dy) * ch / height);
        const u32 y1 = maximum(y0 + 1, cy + static_cast<u32>(static_cast<u64>(dy + 1) * ch / height));
        for (u32 dx = 0; dx < width; ++dx) {
            const u32 x0 = cx + static_cast<u32>(static_cast<u64>(dx) * cw / width);
            const u32 x1 = maximum(x0 + 1, cx + static_cast<u32>(static_cast<u64>(dx + 1) * cw / width));
            u64 sum[4]{}, count = 0;
            for (u32 y = y0; y < y1; ++y)
                for (u32 x = x0; x < x1; ++x) {
                    u32 p[4];
                    pixel(image, scanlines, x, y, p);
                    sum[0] += static_cast<u64>(p[0]) * p[3];
                    sum[1] += static_cast<u64>(p[1]) * p[3];
                    sum[2] += static_cast<u64>(p[2]) * p[3];
                    sum[3] += p[3];
                    ++count;
                }
            u8* out = output + (static_cast<u64>(dy) * width + dx) * 4;
            for (u32 c = 0; c < 3; ++c)
                out[c] = sum[3] ? static_cast<u8>((sum[c] + sum[3] / 2) / sum[3]) : 0;
            out[3] = static_cast<u8>((sum[3] + count / 2) / count);
        }
    }
}

constexpr u32 kStoredBlock = 65'535;

u64 stored_png_bytes(u32 width, u32 height) noexcept {
    const u64 raw = static_cast<u64>(height) * (1ull + static_cast<u64>(width) * 4);
    const u64 zlib = 2 + raw + 5 * ((raw + kStoredBlock - 1) / kStoredBlock) + 4;
    return 8 + 25 + 12 + zlib + 12;
}

// RGBA8 PNG with stored (uncompressed) deflate blocks.
u64 encode_stored_png(const u8* rgba, u32 width, u32 height, u8* out) noexcept {
    const u64 total = stored_png_bytes(width, height);
    const u64 raw = static_cast<u64>(height) * (1ull + static_cast<u64>(width) * 4);
    const u64 zlib = 2 + raw + 5 * ((raw + kStoredBlock - 1) / kStoredBlock) + 4;
    copy_bytes(out, kPngSignature, 8);
    u8* ihdr = out + 8;
    byte_store_be32(ihdr, 13);
    copy_bytes(ihdr + 4, "IHDR", 4);
    byte_store_be32(ihdr + 8, width);
    byte_store_be32(ihdr + 12, height);
    ihdr[16] = 8;
    ihdr[17] = 6;
    ihdr[18] = ihdr[19] = ihdr[20] = 0;
    byte_store_be32(ihdr + 21, png_crc32(ihdr + 4, 17));
    u8* idat = ihdr + 25;
    byte_store_be32(idat, static_cast<u32>(zlib));
    copy_bytes(idat + 4, "IDAT", 4);
    u8* z = idat + 8;
    z[0] = 0x78;
    z[1] = 0x01;
    u64 cursor = 2, emitted = 0;
    u32 adler_a = 1, adler_b = 0, row = 0, column = 0;
    const u32 row_bytes = 1 + width * 4;
    while (emitted < raw) {
        const u32 block = static_cast<u32>(minimum<u64>(kStoredBlock, raw - emitted));
        z[cursor++] = emitted + block == raw ? 1 : 0;
        z[cursor++] = static_cast<u8>(block);
        z[cursor++] = static_cast<u8>(block >> 8);
        z[cursor++] = static_cast<u8>(~block);
        z[cursor++] = static_cast<u8>(~block >> 8);
        for (u32 i = 0; i < block;) {
            // Reduced initial sums and at most 5552 bytes keep both sums in u32.
            const u32 end = i + minimum<u32>(5552, block - i);
            for (; i < end; ++i) {
                const u8 value = column ? rgba[(static_cast<u64>(row) * width * 4) + column - 1] : 0;
                z[cursor++] = value;
                adler_a += value;
                adler_b += adler_a;
                if (++column == row_bytes) {
                    column = 0;
                    ++row;
                }
            }
            adler_a %= 65'521;
            adler_b %= 65'521;
        }
        emitted += block;
    }
    byte_store_be32(z + cursor, adler_b << 16 | adler_a);
    cursor += 4;
    byte_store_be32(z + cursor, png_crc32(idat + 4, static_cast<u32>(4 + zlib)));
    u8* iend = z + cursor + 4;
    byte_store_be32(iend, 0);
    copy_bytes(iend + 4, "IEND", 4);
    byte_store_be32(iend + 8, png_crc32(iend + 4, 4));
    return static_cast<u64>(iend + 12 - out) == total ? total : 0;
}

// Width:height of each use: server banners 16:9, profile banners 17:6
// (680x240), everything else square.
void aspect(GuildImageProfile profile, u64* aw, u64* ah) noexcept {
    *aw = profile == GuildImageProfile::banner ? 16 : profile == GuildImageProfile::profile_banner ? 17 : 1;
    *ah = profile == GuildImageProfile::banner ? 9 : profile == GuildImageProfile::profile_banner ? 6 : 1;
}

// Largest re-encoded size of each use.
void maximum_size(GuildImageProfile profile, u32* width, u32* height) noexcept {
    *width = *height = 0;
    switch (profile) {
    case GuildImageProfile::icon: *width = *height = 1'024; break;
    case GuildImageProfile::banner: *width = 1'280, *height = 720; break;
    case GuildImageProfile::profile_banner: *width = 1'360, *height = 480; break;
    case GuildImageProfile::emoji:
    case GuildImageProfile::role_icon: *width = *height = 128; break;
    case GuildImageProfile::sticker: *width = *height = kGuildStickerDimension; break;
    default: break;
    }
}

bool aspect_matches(GuildImageProfile profile, u32 width, u32 height) noexcept {
    u64 aw = 1, ah = 1;
    aspect(profile, &aw, &ah);
    const u64 a = width * ah, b = height * aw;
    // Within one percent counts as the right shape.
    return (a > b ? a - b : b - a) * 100 <= maximum(a, b);
}

GuildImageInfo fail(GuildImageInfo info, GuildImageError error) noexcept {
    info.error = error;
    info.kind = GuildImageKind::none;
    info.width = info.height = info.bytes = 0;
    info.resized = false;
    return info;
}

} // namespace

GuildImageKind guild_image_sniff(ConstSlice<u8> s) noexcept {
    if (!byte_range_valid(s) || !s.count)
        return GuildImageKind::none;
    if (s.count >= 8 && bytes_equal(s.data, kPngSignature, 8)) {
        bool apng = false;
        probe_png(s, &apng);
        return apng ? GuildImageKind::apng : GuildImageKind::png;
    }
    if (starts(s, 0, "GIF87a", 6) || starts(s, 0, "GIF89a", 6))
        return GuildImageKind::gif;
    if (s.count >= 3 && s[0] == 0xFF && s[1] == 0xD8 && s[2] == 0xFF)
        return GuildImageKind::jpeg;
    if (starts(s, 0, "RIFF", 4) && starts(s, 8, "WEBP", 4))
        return GuildImageKind::webp;
    return lottie(s) ? GuildImageKind::lottie : GuildImageKind::none;
}

void guild_image_release(GuildImageBuffer* buffer) noexcept {
    if (!buffer)
        return;
    if (buffer->data) {
        secure_clear_bytes(buffer->data, buffer->capacity);
        release(buffer->data);
    }
    reset_value(buffer);
}

GuildImageInfo guild_image_prepare(GuildImageProfile profile, ConstSlice<u8> source, GuildImageBuffer* buffer,
                                   GuildPreparedImage* prepared) noexcept {
    GuildImageInfo info{};
    info.source_bytes = source.count;
    if (!buffer || !prepared || profile >= GuildImageProfile::count || !byte_range_valid(source) || !source.count ||
        source.count > kGuildImageSourceMaximumBytes)
        return fail(info, GuildImageError::input);
    reset_value(buffer);
    reset_value(prepared);
    const GuildImageKind kind = guild_image_sniff(source);
    info.source_kind = kind;
    if (kind == GuildImageKind::none || (kind == GuildImageKind::lottie && profile != GuildImageProfile::sticker))
        return fail(info, GuildImageError::format);
    const u32 limit = guild_image_limit(profile);
    if (kind == GuildImageKind::lottie) {
        if (source.count > limit)
            return fail(info, GuildImageError::too_large);
        info.animated = true;
        info.kind = kind;
        info.bytes = source.count;
        *prepared = {source, kind};
        return info;
    }
    const Shape shape = probe(kind, source);
    if (!shape.ok)
        return fail(info, GuildImageError::format);
    info.source_width = shape.width;
    info.source_height = shape.height;
    info.animated = shape.animated;
    if (!guild_image_kind_allowed(profile, kind) || (profile == GuildImageProfile::role_icon && shape.animated))
        return fail(info, GuildImageError::kind);
    const u32 minimum_side = profile == GuildImageProfile::role_icon ? 64
                             : profile == GuildImageProfile::sticker ? kGuildStickerDimension
                                                                     : 1;
    if (shape.width < minimum_side || shape.height < minimum_side)
        return fail(info, GuildImageError::dimensions);
    u32 max_width = 0, max_height = 0;
    maximum_size(profile, &max_width, &max_height);
    const bool exact_sticker = shape.width == kGuildStickerDimension && shape.height == kGuildStickerDimension;
    const bool static_png = kind == GuildImageKind::png;
    const bool needs = static_png && (!aspect_matches(profile, shape.width, shape.height) ||
                                      shape.width > max_width || shape.height > max_height || source.count > limit ||
                                      (profile == GuildImageProfile::sticker && !exact_sticker));
    const auto as_is = [&]() noexcept {
        if (source.count > limit)
            return fail(info, GuildImageError::too_large);
        if (profile == GuildImageProfile::sticker && !exact_sticker)
            return fail(info, GuildImageError::dimensions);
        info.kind = kind;
        info.width = shape.width;
        info.height = shape.height;
        info.bytes = source.count;
        *prepared = {source, kind};
        return info;
    };
    if (!needs)
        return as_is();
    PngImage image{};
    if (!png_parse(source, &image).ok() || image.width != shape.width || image.height != shape.height)
        return source.count <= limit && profile != GuildImageProfile::sticker ? as_is()
                                                                              : fail(info, GuildImageError::decode);
    // Centre crop to the use's aspect, then shrink to its maximum size.
    u32 cw = image.width, ch = image.height;
    u64 aw = 1, ah = 1;
    aspect(profile, &aw, &ah);
    if (static_cast<u64>(cw) * ah > static_cast<u64>(ch) * aw)
        cw = static_cast<u32>(static_cast<u64>(ch) * aw / ah);
    else
        ch = static_cast<u32>(static_cast<u64>(cw) * ah / aw);
    cw = maximum(cw, 1u);
    ch = maximum(ch, 1u);
    const u32 cx = (image.width - cw) / 2, cy = (image.height - ch) / 2;
    u32 width = minimum(cw, max_width);
    u32 height = maximum(1u, static_cast<u32>((static_cast<u64>(ch) * width + cw / 2) / cw));
    if (height > max_height) {
        height = max_height;
        width = maximum(1u, static_cast<u32>((static_cast<u64>(cw) * height + ch / 2) / ch));
    }
    u8* scanlines = allocate(image.inflated_bytes);
    u8* rgba = allocate(static_cast<u64>(width) * height * 4);
    const u64 png_bytes = stored_png_bytes(width, height);
    u8* output = png_bytes <= limit ? allocate(png_bytes) : nullptr;
    if (!scanlines || !rgba || !output) {
        release(scanlines);
        release(rgba);
        release(output);
        return fail(info, png_bytes > limit ? GuildImageError::too_large : GuildImageError::memory);
    }
    DeflateWorkspace workspace{};
    const auto decoded = png_decode_parsed_scanlines(&image, &workspace, {scanlines, image.inflated_bytes});
    if (decoded.ok())
        resample(image, scanlines, cx, cy, cw, ch, width, height, rgba);
    release(scanlines);
    const u64 written = decoded.ok() ? encode_stored_png(rgba, width, height, output) : 0;
    secure_clear_bytes(rgba, static_cast<SIZE_T>(static_cast<u64>(width) * height * 4));
    release(rgba);
    if (!written) {
        release(output);
        return fail(info, GuildImageError::decode);
    }
    *buffer = {output, static_cast<u32>(png_bytes), static_cast<u32>(written)};
    *prepared = {{output, static_cast<u32>(written)}, GuildImageKind::png};
    info.kind = GuildImageKind::png;
    info.width = width;
    info.height = height;
    info.bytes = static_cast<u32>(written);
    info.resized = true;
    return info;
}

} // namespace maicord
