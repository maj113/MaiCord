#include "maicord/app/local_preview_thumbnail.hpp"
#include "maicord/app/profile_image.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/gif.hpp"
#include "maicord/media/png.hpp"
#include "maicord/media/png_encode.hpp"
#include "maicord/media/webp.hpp"
#include "maicord/media/webp_thumbnail.hpp"
#include "maicord/model/image_crop.hpp"
#include "media_fixture.hpp"
#include "webp_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace profile_image_test {

u32 failures;

void write_text(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes])
        ++bytes;
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
}

void check(bool valid, const char* label) noexcept {
    if (valid)
        return;
    ++failures;
    write_text(label);
    write_text("\n");
}

// Owned scratch, released on scope exit.
struct Memory {
    u8* data = nullptr;
    u32 bytes = 0;
    explicit Memory(u64 size) noexcept {
        data = static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT,
                                             PAGE_READWRITE));
        bytes = data ? static_cast<u32>(size) : 0;
    }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    ~Memory() noexcept {
        if (data)
            VirtualFree(data, 0, MEM_RELEASE);
    }
};

bool close_to(float value, float expected, float tolerance = 0.01f) noexcept {
    const float d = value - expected;
    return d <= tolerance && d >= -tolerance;
}
bool within(u32 value, u32 expected, u32 tolerance) noexcept {
    return value + tolerance >= expected && value <= expected + tolerance;
}
u32 lcg(u32* state) noexcept {
    *state = *state * 1'664'525u + 1'013'904'223u;
    return *state >> 16;
}

// Crop geometry.

bool output_is(const ImageCropOutput& o, u32 x, u32 y, u32 w, u32 h, u32 ow, u32 oh, u32 tolerance = 0) noexcept {
    return within(o.x, x, tolerance) && within(o.y, y, tolerance) && within(o.width, w, tolerance) &&
           within(o.height, h, tolerance) && within(o.output_width, ow, tolerance) &&
           within(o.output_height, oh, tolerance);
}

void test_crop_layout() noexcept {
    // Avatars: the image at the editor height, the mask its shorter side.
    ImageCropState s = image_crop_begin(ImageCropUse::avatar, 1000, 2000, 430, 350);
    check(close_to(s.base_width, 175) && close_to(s.base_height, 350) && close_to(s.mask_width, 175) && close_to(s.mask_height, 175),
          "a tall avatar is laid out at the editor height with a mask of its width");
    check(image_crop_bound_x(s) == 0 && close_to(image_crop_bound_y(s), 87.5f), "a tall avatar pans vertically only");
    check(output_is(image_crop_output(s), 0, 500, 1000, 1000, 1000, 1000), "a tall avatar crops its centre square");
    s = image_crop_begin(ImageCropUse::avatar, 2000, 1000, 430, 350);
    check(close_to(s.base_width, 700) && close_to(s.mask_width, 350) &&
              output_is(image_crop_output(s), 500, 0, 1000, 1000, 1000, 1000),
          "a wide avatar crops its centre square");
    s = image_crop_begin(ImageCropUse::avatar, 4000, 4000, 430, 350);
    check(output_is(image_crop_output(s), 0, 0, 4000, 4000, 1024, 1024), "a large square avatar is capped at 1024");
    s = image_crop_begin(ImageCropUse::avatar, 200, 200, 430, 350);
    check(output_is(image_crop_output(s), 0, 0, 200, 200, 200, 200), "small avatars are never upscaled");

    // Banners: fitted to 17:6 at most the editor width (<= 432) wide.
    s = image_crop_begin(ImageCropUse::banner, 4000, 1000, 430, 350);
    check(close_to(s.base_width, 607.06f, 0.05f) && close_to(s.base_height, 151.76f, 0.05f) && close_to(s.mask_width, 430) &&
              close_to(s.mask_height, 151.76f, 0.05f),
          "a wide banner is fitted to the crop height");
    check(output_is(image_crop_output(s), 583, 0, 2833, 1000, 2400, 848, 1),
          "a wide banner's crop is capped per axis at 2400x848");
    s = image_crop_begin(ImageCropUse::banner, 1000, 2000, 430, 350);
    check(close_to(s.base_width, 175) && close_to(s.base_height, 350) && close_to(s.mask_width, 175) &&
              close_to(s.mask_height, 61.76f, 0.05f),
          "a tall banner keeps its width as the mask width");
    check(output_is(image_crop_output(s), 0, 824, 1000, 353, 1000, 353, 1), "a tall banner crops a centred 17:6 strip");
    s = image_crop_begin(ImageCropUse::banner, 4000, 1000, 1000, 350);
    check(close_to(s.mask_width, kImageCropMaximumMaskWidth), "the banner mask is at most 432 wide");
}

void test_crop_interaction() noexcept {
    ImageCropState s = image_crop_begin(ImageCropUse::avatar, 1000, 2000, 430, 350);
    image_crop_pan(&s, 500, 1000);
    check(s.offset_x == 0 && close_to(s.offset_y, 87.5f), "panning clamps to the covered range");
    image_crop_set_zoom(&s, 3);
    check(s.zoom == kImageCropMaximumZoom && close_to(image_crop_bound_x(s), 87.5f) && close_to(image_crop_bound_y(s), 262.5f),
          "zoom clamps to 2 and widens the bounds");
    image_crop_pan(&s, 1000, 1000);
    check(close_to(s.offset_x, 87.5f) && close_to(s.offset_y, 262.5f), "zoomed pans reach the zoomed bounds");
    image_crop_set_zoom(&s, 0.5f);
    check(s.zoom == kImageCropMinimumZoom && s.offset_x == 0 && close_to(s.offset_y, 87.5f),
          "zooming out re-clamps the offset");
    check(image_crop_edited(s), "an offset counts as an edit");
    image_crop_reset(&s);
    check(!image_crop_edited(s) && s.offset_y == 0, "reset restores the initial layout");

    // Zoom 2 on a square: the middle half, shifted by the offset.
    s = image_crop_begin(ImageCropUse::avatar, 1000, 1000, 430, 350);
    image_crop_set_zoom(&s, 2);
    check(output_is(image_crop_output(s), 250, 250, 500, 500, 500, 500), "zoom 2 crops the middle half");
    image_crop_pan(&s, 87.5f, 0);
    check(output_is(image_crop_output(s), 125, 250, 500, 500, 500, 500), "moving the image right crops further left");

    // Rotation turns the offset with the image and swaps the layout.
    s = image_crop_begin(ImageCropUse::avatar, 1000, 1000, 430, 350);
    image_crop_set_zoom(&s, 2);
    s.offset_x = 10;
    s.offset_y = 20;
    image_crop_rotate(&s);
    check(s.rotation == 90 && close_to(s.offset_x, -20) && close_to(s.offset_y, 10), "rotating maps the offset (x,y) to (-y,x)");
    s = image_crop_begin(ImageCropUse::avatar, 1000, 2000, 430, 350);
    image_crop_rotate(&s);
    check(close_to(s.base_width, 700) && close_to(s.base_height, 350) && close_to(s.mask_width, 350),
          "a rotated tall image is laid out wide");
    image_crop_pan(&s, 1000, 0);
    const ImageCropOutput turned = image_crop_output(s);
    check(output_is(turned, 0, 1000, 1000, 1000, 1000, 1000) && turned.rotation == 90,
          "the visual left of a clockwise turn is the source bottom");
    for (u32 i = 0; i < 3; ++i)
        image_crop_rotate(&s);
    check(s.rotation == 0 && close_to(s.base_width, 175), "four turns restore the layout");
    s = image_crop_begin(ImageCropUse::banner, 1000, 2000, 430, 350);
    image_crop_rotate(&s);
    check(output_is(image_crop_output(s), 147, 0, 706, 2000, 2000, 706, 1),
          "a rotated banner's output is in the visual orientation");

    // A resize keeps the offset ratio within the new bounds.
    s = image_crop_begin(ImageCropUse::avatar, 1000, 2000, 430, 350);
    image_crop_pan(&s, 0, 50);
    image_crop_resize(&s, 430, image_crop_editor_height(700));
    check(close_to(s.base_height, 200) && close_to(s.mask_width, 100) && close_to(s.offset_y, 28.571f, 0.01f),
          "a short window relays out at 200 and keeps the offset ratio");
}

// PNG encoder.

bool inflate_matches(ConstSlice<u8> compressed, ConstSlice<u8> expected) noexcept {
    Memory out(expected.count + 16);
    DeflateWorkspace workspace{};
    const DeflateInputSpan span{compressed.data, compressed.count};
    const auto result = zlib_inflate({&span, 1}, {out.data, expected.count + 16}, expected.count, &workspace);
    return result.ok() && result.output_bytes == expected.count && bytes_equal(out.data, expected.data, expected.count);
}

// Decodes a PNG of at most 2048 px to straight RGBA8.
bool decode_png(ConstSlice<u8> png, Memory* rgba, u32* width, u32* height, u8* color_type) noexcept {
    PngImage image{};
    if (!png_parse(png, &image).ok())
        return false;
    const u32 bytes = png_decode_buffer_bytes(image);
    Memory scanlines(bytes);
    DeflateWorkspace workspace{};
    if (!scanlines.data || !png_decode_parsed_scanlines(&image, &workspace, {scanlines.data, bytes}).ok() ||
        static_cast<u64>(image.width) * image.height * 4 > rgba->bytes)
        return false;
    for (u32 y = 0; y < image.height; ++y)
        if (!local_thumbnail_png_row(image, scanlines.data + static_cast<u64>(y) * image.row_stride + 1,
                                     rgba->data + static_cast<u64>(y) * image.width * 4))
            return false;
    *width = image.width;
    *height = image.height;
    *color_type = image.color_type;
    return true;
}

u32 encode_png(const u8* rgba, u32 width, u32 height, Memory* out) noexcept {
    Memory workspace(png_encode_workspace_bytes(width, height));
    return png_encode_rgba8(rgba, width, height, {workspace.data, workspace.bytes}, {out->data, out->bytes});
}

void test_zlib() noexcept {
    Memory workspace(kZlibCompressWorkspaceBytes);
    Memory input(300'000);
    Memory output(zlib_compress_bound(300'000));
    const Slice<u8> ws{workspace.data, workspace.bytes};
    u32 empty = zlib_compress({nullptr, 0}, ws, {output.data, output.bytes});
    check(empty && empty <= zlib_compress_bound(0) && inflate_matches({output.data, empty}, {input.data, 0}),
          "an empty zlib stream round-trips");
    input.data[0] = 'a';
    u32 bytes = zlib_compress({input.data, 1}, ws, {output.data, output.bytes});
    check(bytes && inflate_matches({output.data, bytes}, {input.data, 1}), "a one-byte zlib stream round-trips");
    // Repetition across many 64 KiB blocks, with matches into earlier blocks.
    for (u32 i = 0; i < 300'000; ++i)
        input.data[i] = static_cast<u8>("profile banner crop "[i % 20] + (i / 7000 % 3));
    bytes = zlib_compress({input.data, 300'000}, ws, {output.data, output.bytes});
    check(bytes && bytes < 30'000 && inflate_matches({output.data, bytes}, {input.data, 300'000}),
          "repetitive input compresses across blocks and round-trips");
    // Noise falls back to stored blocks and stays within the bound.
    u32 seed = 7;
    for (u32 i = 0; i < 200'000; ++i)
        input.data[i] = static_cast<u8>(lcg(&seed));
    bytes = zlib_compress({input.data, 200'000}, ws, {output.data, output.bytes});
    check(bytes && bytes <= zlib_compress_bound(200'000) && inflate_matches({output.data, bytes}, {input.data, 200'000}),
          "incompressible input stays within the stored bound");
    // Skewed symbols build deep (length-limited) Huffman codes.
    for (u32 i = 0; i < 65'000; ++i) {
        const u32 r = lcg(&seed);
        input.data[i] = static_cast<u8>(r % 97 == 0 ? r >> 3 : r % 5 == 0 ? 1 : 0);
    }
    bytes = zlib_compress({input.data, 65'000}, ws, {output.data, output.bytes});
    check(bytes && inflate_matches({output.data, bytes}, {input.data, 65'000}), "skewed input round-trips");
    check(!zlib_compress({input.data, 1000}, ws, {output.data, 20}), "a short output fails instead of overflowing");
}

void test_png_encoder() noexcept {
    constexpr u32 w = 37, h = 23;
    Memory source(w * h * 4), decoded(w * h * 4), png(png_encode_bound(w, h));
    u32 seed = 3;
    for (u32 i = 0; i < w * h; ++i) {
        source.data[i * 4] = static_cast<u8>(i * 7);
        source.data[i * 4 + 1] = static_cast<u8>(i / w * 11);
        source.data[i * 4 + 2] = static_cast<u8>(lcg(&seed));
        source.data[i * 4 + 3] = static_cast<u8>(i % 5 ? 255 : i * 3);
    }
    u32 width = 0, height = 0;
    u8 type = 0;
    u32 bytes = encode_png(source.data, w, h, &png);
    check(bytes && decode_png({png.data, bytes}, &decoded, &width, &height, &type) && width == w && height == h &&
              type == 6 && bytes_equal(source.data, decoded.data, w * h * 4),
          "translucent pixels encode as RGBA8 PNG and decode exactly");
    for (u32 i = 0; i < w * h; ++i)
        source.data[i * 4 + 3] = 255;
    bytes = encode_png(source.data, w, h, &png);
    check(bytes && decode_png({png.data, bytes}, &decoded, &width, &height, &type) && type == 2 &&
              bytes_equal(source.data, decoded.data, w * h * 4),
          "opaque pixels encode as RGB8 PNG and decode exactly");

    // A photograph-like 1024 x 1024 still: smooth fields with sensor noise.
    constexpr u32 n = 1024;
    Memory photo(n * n * 4), round(n * n * 4), big(png_encode_bound(n, n));
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            u8* p = photo.data + (y * n + x) * 4;
            const u32 noise = lcg(&seed);
            const u32 dx = x > 512 ? x - 512 : 512 - x, dy = y > 400 ? y - 400 : 400 - y;
            p[0] = static_cast<u8>((x + y) / 8 + (noise & 3));
            p[1] = static_cast<u8>(255 - minimum(255u, (dx * dx + dy * dy) >> 11) + (noise >> 2 & 3));
            p[2] = static_cast<u8>(96 + (x * y >> 14) + (noise >> 4 & 3));
            p[3] = 255;
        }
    bytes = encode_png(photo.data, n, n, &big);
    const u64 raw = static_cast<u64>(n) * (1 + n * 4);
    const u64 stored = 8 + 25 + 12 + 2 + raw + 5 * ((raw + 65'534) / 65'535) + 4 + 12;
    check(bytes && stored * 2 >= bytes * 3, "a 1024 px photo compresses 1.5x below a stored PNG");
    check(bytes && decode_png({big.data, bytes}, &round, &width, &height, &type) && type == 2 &&
              bytes_equal(photo.data, round.data, n * n * 4),
          "the 1024 px photo decodes exactly");
}

// Probe and crop.

u32 test_rgba(u32 x, u32 y) noexcept {
    const u32 a = x == 2 && y == 1 ? 128u : x == 5 && y == 3 ? 0u : 255u;
    return (x * 30 + 5) | (y * 40 + 7) << 8 | ((x * y * 11 + 60) & 0xFFu) << 16 | a << 24;
}
u32 pixel_at(const u8* rgba, u32 width, u32 x, u32 y) noexcept {
    return byte_load_le32(rgba + (static_cast<u64>(y) * width + x) * 4);
}
// The source pixel shown at output (X, Y) of a crop rotated clockwise.
void unrotate(u16 rotation, u32 ow, u32 oh, u32 X, u32 Y, u32* u, u32* v) noexcept {
    const bool turned = rotation % 180 != 0;
    const u32 uw = turned ? oh : ow, uh = turned ? ow : oh;
    *u = X, *v = Y;
    if (rotation == 90)
        *u = Y, *v = uh - 1 - X;
    else if (rotation == 180)
        *u = uw - 1 - X, *v = uh - 1 - Y;
    else if (rotation == 270)
        *u = uw - 1 - Y, *v = X;
}
ImageCropOutput crop_rect(u32 x, u32 y, u32 w, u32 h, u16 rotation, u32 ow = 0, u32 oh = 0) noexcept {
    const bool turned = rotation % 180 != 0;
    ImageCropOutput o{};
    o.x = x;
    o.y = y;
    o.width = w;
    o.height = h;
    o.rotation = rotation;
    o.output_width = ow ? ow : turned ? h : w;
    o.output_height = oh ? oh : turned ? w : h;
    return o;
}

// Crops, then decodes the PNG result.
bool crop_png(ConstSlice<u8> source, const ImageCropOutput& crop, Memory* rgba, ProfileImageInfo* info,
              u8* type = nullptr) noexcept {
    GuildImageBuffer out{};
    *info = profile_image_crop(source, crop, &out);
    u32 w = 0, h = 0;
    u8 t = 0;
    const bool ok = info->error == ProfileImageError::none && info->output_kind == GuildImageKind::png &&
                    out.data && out.bytes == info->bytes &&
                    decode_png({out.data, out.bytes}, rgba, &w, &h, &t) && w == info->output_width &&
                    h == info->output_height;
    if (type)
        *type = t;
    guild_image_release(&out);
    return ok;
}

void test_png_crop() noexcept {
    constexpr u32 w = 7, h = 5;
    u8 pixels[w * h * 4];
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            byte_store_le32(pixels + (y * w + x) * 4, test_rgba(x, y));
    Memory png(png_encode_bound(w, h)), rgba(64 * 64 * 4);
    const u32 bytes = encode_png(pixels, w, h, &png);
    const ConstSlice<u8> source{png.data, bytes};
    const ProfileImageInfo probe = profile_image_probe(source);
    check(probe.error == ProfileImageError::none && probe.kind == GuildImageKind::png && probe.width == w &&
              probe.height == h && probe.frames == 1 && !probe.animated && probe.output_kind == GuildImageKind::png,
          "PNG probe reports size and kind");
    // Pixel-exact crop and rotation when the output equals the crop.
    constexpr u16 rotations[4] = {0, 90, 180, 270};
    for (const u16 rotation : rotations) {
        const ImageCropOutput crop = crop_rect(2, 1, 4, 3, rotation);
        ProfileImageInfo info{};
        u8 type = 0;
        bool exact = crop_png(source, crop, &rgba, &info, &type) && type == 6;
        for (u32 Y = 0; exact && Y < info.output_height; ++Y)
            for (u32 X = 0; X < info.output_width; ++X) {
                u32 u = 0, v = 0;
                unrotate(rotation, info.output_width, info.output_height, X, Y, &u, &v);
                u32 expected = test_rgba(crop.x + u, crop.y + v);
                if (!(expected >> 24))
                    expected = 0;
                exact = exact && pixel_at(rgba.data, info.output_width, X, Y) == expected;
            }
        check(exact, rotation == 0    ? "an unscaled PNG crop is pixel exact"
                     : rotation == 90  ? "a 90 degree crop is pixel exact"
                     : rotation == 180 ? "a 180 degree crop is pixel exact"
                                       : "a 270 degree crop is pixel exact");
    }
    // Opaque crops become RGB8.
    ProfileImageInfo info{};
    u8 type = 0;
    check(crop_png(source, crop_rect(0, 0, 2, 2, 0), &rgba, &info, &type) && type == 2 &&
              pixel_at(rgba.data, 2, 1, 1) == test_rgba(1, 1),
          "an opaque crop is written as RGB8");

    // Downscaling averages 2x2 blocks exactly.
    u8 blocks[8 * 8 * 4];
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 8; ++x)
            byte_store_le32(blocks + (y * 8 + x) * 4, (x / 2 * 60) | (y / 2 * 60) << 8 | 200u << 16 | 255u << 24);
    u32 b = encode_png(blocks, 8, 8, &png);
    bool averaged = crop_png({png.data, b}, crop_rect(0, 0, 8, 8, 0, 4, 4), &rgba, &info);
    for (u32 y = 0; averaged && y < 4; ++y)
        for (u32 x = 0; x < 4; ++x)
            averaged = averaged && pixel_at(rgba.data, 4, x, y) == ((x * 60) | (y * 60) << 8 | 200u << 16 | 255u << 24);
    check(averaged, "a 2x downscale averages each 2x2 block");
    // Colour is weighted by alpha: transparent pixels do not darken.
    const u32 pair[2] = {0xFF0000FFu, 0x00FF0000u};
    b = encode_png(reinterpret_cast<const u8*>(pair), 2, 1, &png);
    check(crop_png({png.data, b}, crop_rect(0, 0, 2, 1, 0, 1, 1), &rgba, &info) &&
              pixel_at(rgba.data, 1, 0, 0) == 0x800000FFu,
          "downscaling weights colour by alpha");
    // Non-integer ratios keep flat colour flat.
    u8 flat[9 * 9 * 4];
    for (u32 i = 0; i < 81; ++i)
        byte_store_le32(flat + i * 4, 0xFF1E140Au);
    b = encode_png(flat, 9, 9, &png);
    bool uniform = crop_png({png.data, b}, crop_rect(0, 0, 9, 9, 90, 4, 4), &rgba, &info);
    for (u32 i = 0; uniform && i < 16; ++i)
        uniform = byte_load_le32(rgba.data + i * 4) == 0xFF1E140Au;
    check(uniform, "a 9 to 4 resample keeps a flat colour exact");
    // 10x1 to 3x1: the height allows no box stage, so exact area averages.
    u8 ramp[10 * 4];
    for (u32 x = 0; x < 10; ++x)
        byte_store_le32(ramp + x * 4, (x * 20) | 255u << 24);
    b = encode_png(ramp, 10, 1, &png);
    check(crop_png({png.data, b}, crop_rect(0, 0, 10, 1, 0, 3, 1), &rgba, &info) && within(rgba.data[0], 24, 1) &&
              within(rgba.data[4], 90, 1) && within(rgba.data[8], 156, 1),
          "a 10 to 3 resample averages exact areas");
    // 10x10 to 3x3 boxes 3x3 first (20, 80, 140, 180 over 3, 3, 3 and 1
    // columns), then area-weights those boxes.
    u8 square[10 * 10 * 4];
    for (u32 i = 0; i < 100; ++i)
        byte_store_le32(square + i * 4, (i % 10 * 20) | 255u << 24);
    b = encode_png(square, 10, 10, &png);
    check(crop_png({png.data, b}, crop_rect(0, 0, 10, 10, 0, 3, 3), &rgba, &info) && within(rgba.data[0], 26, 1) &&
              within(rgba.data[4], 92, 1) && within(rgba.data[8], 152, 1),
          "a 10 to 3 resample area-averages the integer boxes");

    // An APNG crops its default image.
    Memory apng(4096), scratch(4096);
    ApngFixtureResult fixture{};
    if (build_rgba8_apng_fixture({apng.data, apng.bytes}, {scratch.data, scratch.bytes}, &fixture)) {
        const ConstSlice<u8> animation{apng.data, fixture.encoded_bytes};
        const ProfileImageInfo a = profile_image_probe(animation);
        check(a.error == ProfileImageError::none && a.kind == GuildImageKind::apng && a.animated &&
                  a.frames == fixture.frame_count && a.output_kind == GuildImageKind::png,
              "APNG probe reports its frames");
        check(crop_png(animation, crop_rect(1, 1, 2, 2, 0), &rgba, &info) &&
                  pixel_at(rgba.data, 2, 1, 0) == apng_fixture_source_rgba(0, 2, 1),
              "an APNG crops to a still of its default image");
    } else {
        check(false, "APNG fixture builds");
    }
}

void test_jpeg_crop() noexcept {
    u8 base[256], oriented[512];
    JpegFixtureResult fixture{};
    u32 bytes = 0;
    if (!build_baseline_jpeg_fixture({base, sizeof(base)}, true, &fixture) ||
        !build_jpeg_exif_orientation_fixture({base, fixture.encoded_bytes}, {oriented, sizeof(oriented)}, 6, false,
                                             &bytes)) {
        check(false, "JPEG fixtures build");
        return;
    }
    const ConstSlice<u8> source{oriented, bytes};
    const ProfileImageInfo probe = profile_image_probe(source);
    check(probe.error == ProfileImageError::none && probe.kind == GuildImageKind::jpeg && probe.width == 8 &&
              probe.height == 16 && probe.frames == 1,
          "JPEG probe applies the EXIF orientation to the size");
    const u32 expected = jpeg_fixture_rgba();
    const auto flat = [&](const Memory& rgba, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i)
            for (u32 c = 0; c < 4; ++c)
                if (!within(rgba.data[i * 4 + c], expected >> (c * 8) & 0xFFu, 3))
                    return false;
        return true;
    };
    Memory rgba(64 * 64 * 4);
    ProfileImageInfo info{};
    check(crop_png(source, crop_rect(0, 0, 8, 16, 0), &rgba, &info) && info.output_width == 8 &&
              info.output_height == 16 && flat(rgba, 128),
          "an oriented JPEG crops in display orientation");
    check(crop_png(source, crop_rect(0, 8, 8, 8, 90), &rgba, &info) && info.output_width == 8 && flat(rgba, 64),
          "a JPEG sub-rectangle crops and rotates");
    check(crop_png(source, crop_rect(0, 0, 8, 16, 270, 8, 4), &rgba, &info) && info.output_width == 8 &&
              info.output_height == 4 && flat(rgba, 32),
          "a JPEG downscale decodes through the DCT reduction");
    check(crop_png({base, fixture.encoded_bytes}, crop_rect(4, 0, 12, 8, 180), &rgba, &info) &&
              info.output_width == 12 && flat(rgba, 96),
          "an unoriented JPEG crops");
}

void test_webp_crop() noexcept {
    Memory rgba(64 * 64 * 4);
    ProfileImageInfo info{};
    // Lossless: exact against the decoder's own output.
    {
        const ConstSlice<u8> source{kWebpGradient, sizeof(kWebpGradient)};
        const ProfileImageInfo probe = profile_image_probe(source);
        check(probe.error == ProfileImageError::none && probe.kind == GuildImageKind::webp && probe.width == 16 &&
                  probe.height == 16,
              "lossless WebP probe");
        Memory workspace(sizeof(Vp8lWorkspace)), reference(16 * 16 * 4);
        WebpImage image{};
        const bool decoded = webp_lossless_decode_rgba8(source, &image, reinterpret_cast<Vp8lWorkspace*>(workspace.data),
                                                        {reference.data, reference.bytes})
                                 .ok();
        bool exact = decoded && crop_png(source, crop_rect(3, 2, 8, 6, 270), &rgba, &info);
        for (u32 Y = 0; exact && Y < info.output_height; ++Y)
            for (u32 X = 0; X < info.output_width; ++X) {
                u32 u = 0, v = 0;
                unrotate(270, info.output_width, info.output_height, X, Y, &u, &v);
                u32 want = pixel_at(reference.data, 16, 3 + u, 2 + v);
                if (!(want >> 24))
                    want = 0;
                exact = exact && pixel_at(rgba.data, info.output_width, X, Y) == want;
            }
        check(exact, "a lossless WebP crop is pixel exact");
    }
    // Lossy: exact against the row decoder.
    {
        const ConstSlice<u8> source{kWebpLossy32x24, sizeof(kWebpLossy32x24)};
        WebpThumbnailLayout layout{};
        Memory workspace(4u << 20), reference(32 * 24 * 4);
        struct Collect {
            u8* rgba;
        } collect{reference.data};
        const bool decoded =
            webp_thumbnail_parse(source, &layout) == WebpThumbnailError::none &&
            webp_thumbnail_decode(
                layout, {workspace.data, workspace.bytes},
                [](void* context, u32 y, const u8* row) noexcept {
                    copy_bytes(static_cast<Collect*>(context)->rgba + y * 32 * 4, row, 32 * 4);
                    return true;
                },
                &collect) == WebpThumbnailError::none;
        bool exact = decoded && crop_png(source, crop_rect(4, 4, 16, 12, 180), &rgba, &info);
        for (u32 Y = 0; exact && Y < 12; ++Y)
            for (u32 X = 0; X < 16; ++X)
                exact = exact && pixel_at(rgba.data, 16, X, Y) == pixel_at(reference.data, 32, 4 + 15 - X, 4 + 11 - Y);
        check(exact, "a lossy WebP crop is pixel exact");
    }
    // Animated: a still of the first frame.
    {
        Memory encoded(4096);
        AnimatedWebpFixtureResult fixture{};
        if (build_lossless_animated_webp_fixture({encoded.data, encoded.bytes}, &fixture)) {
            const ConstSlice<u8> source{encoded.data, fixture.encoded_bytes};
            const ProfileImageInfo probe = profile_image_probe(source);
            check(probe.error == ProfileImageError::none && probe.animated && probe.frames == fixture.frame_count &&
                      probe.width == 4 && probe.height == 4 && probe.output_kind == GuildImageKind::png,
                  "animated WebP probe reports its frames");
            check(crop_png(source, crop_rect(0, 0, 4, 4, 0), &rgba, &info) &&
                      pixel_at(rgba.data, 4, 3, 3) == animated_webp_fixture_source_rgba(0),
                  "an animated WebP crops to its first frame");
        } else {
            check(false, "animated WebP fixture builds");
        }
    }
}

// GIF.

// Every frame of `encoded` decoded: rectangles, delays, flags and indices.
struct GifFrames {
    GifImage image;
    GifWorkspace* workspace;
    Memory storage{sizeof(GifWorkspace)};
    bool parse(ConstSlice<u8> encoded) noexcept {
        workspace = reinterpret_cast<GifWorkspace*>(storage.data);
        image = {};
        return gif_parse(encoded, &image, workspace).ok();
    }
};

// Each output frame's indices match the source frame under the crop.
bool gif_crop_matches(ConstSlice<u8> source, ConstSlice<u8> output, const ImageCropOutput& crop) noexcept {
    GifFrames in, out;
    if (!in.parse(source) || !out.parse(output) || in.image.frame_count != out.image.frame_count ||
        in.image.play_count != out.image.play_count)
        return false;
    const bool turned = crop.rotation % 180 != 0;
    const u32 ow = turned ? crop.height : crop.width, oh = turned ? crop.width : crop.height;
    if (out.image.canvas_width != ow || out.image.canvas_height != oh)
        return false;
    Memory a(kGifPixelMaximum), b(kGifPixelMaximum);
    for (u32 i = 0; i < in.image.frame_count; ++i) {
        const auto s = gif_decode_parsed_frame(source, in.image, i, in.workspace, {a.data, a.bytes});
        const auto d = gif_decode_parsed_frame(output, out.image, i, out.workspace, {b.data, b.bytes});
        if (!s.ok() || !d.ok() || s.frame.delay_cs != d.frame.delay_cs)
            return false;
        const GifFrame& f = s.frame;
        const u32 x0 = maximum(f.x_offset, crop.x), x1 = minimum(f.x_offset + f.width, crop.x + crop.width);
        const u32 y0 = maximum(f.y_offset, crop.y), y1 = minimum(f.y_offset + f.height, crop.y + crop.height);
        if (x0 >= x1 || y0 >= y1) {
            if (d.frame.width != 1 || d.frame.height != 1 || !(d.frame.flags & gif_frame_transparent) ||
                d.palette.rgba[b.data[0]] >> 24 || d.frame.dispose != GifDispose::none)
                return false;
            continue;
        }
        const u32 area = (x1 - x0) * (y1 - y0);
        if (d.frame.width * d.frame.height != area || d.frame.dispose != f.dispose ||
            (d.frame.flags & gif_frame_transparent) != (f.flags & gif_frame_transparent) ||
            ((f.flags & gif_frame_transparent) && d.frame.transparent_index != f.transparent_index))
            return false;
        for (u32 c = 0; c < s.palette.count; ++c)
            if (s.palette.rgba[c] != d.palette.rgba[c])
                return false;
        for (u32 y = 0; y < d.frame.height; ++y)
            for (u32 x = 0; x < d.frame.width; ++x) {
                u32 u = 0, v = 0;
                unrotate(crop.rotation, ow, oh, d.frame.x_offset + x, d.frame.y_offset + y, &u, &v);
                const u32 sx = crop.x + u, sy = crop.y + v;
                if (sx < x0 || sx >= x1 || sy < y0 || sy >= y1 ||
                    b.data[y * d.frame.width + x] != a.data[(sy - f.y_offset) * f.width + (sx - f.x_offset)])
                    return false;
            }
    }
    return true;
}

// GIF89a of random 8-bit indices coded as literal LZW codes (a clear every
// 254 codes keeps the width at 9 bits); the second frame is transparent.
u32 build_random_gif(u8* out, u32 capacity, u32 w, u32 h, u32 frames, u32 seed) noexcept {
    u32 at = 0;
    const auto put = [&](u32 v) noexcept {
        if (at < capacity)
            out[at] = static_cast<u8>(v);
        ++at;
    };
    const auto put16 = [&](u32 v) noexcept {
        put(v & 0xFF);
        put(v >> 8);
    };
    constexpr u8 header[6] = {'G', 'I', 'F', '8', '9', 'a'};
    for (const u8 c : header)
        put(c);
    put16(w);
    put16(h);
    put(0xF7);
    put(0);
    put(0);
    for (u32 i = 0; i < 256; ++i) {
        put(i);
        put(255 - i);
        put(i * 7);
    }
    constexpr u8 loop[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0, 0, 0};
    for (const u8 c : loop)
        put(c);
    for (u32 f = 0; f < frames; ++f) {
        put(0x21);
        put(0xF9);
        put(4);
        put((f == 1 ? 2u : 1u) << 2 | (f == 1 ? 1u : 0u));
        put16((f + 1) * 3);
        put(7);
        put(0);
        put(0x2C);
        put16(f == 1 ? 3 : 0);
        put16(f == 1 ? 2 : 0);
        put16(f == 1 ? w - 5 : w);
        put16(f == 1 ? h - 4 : h);
        put(0);
        put(8);
        const u32 fw = f == 1 ? w - 5 : w, fh = f == 1 ? h - 4 : h;
        u32 block_at = at, block = 0, bits = 0, since = 0;
        u64 acc = 0;
        put(0);
        const auto emit = [&](u32 code) noexcept {
            acc |= static_cast<u64>(code) << bits;
            bits += 9;
            while (bits >= 8) {
                put(static_cast<u32>(acc & 0xFF));
                acc >>= 8;
                bits -= 8;
                if (++block == 255) {
                    if (block_at < capacity)
                        out[block_at] = 255;
                    block_at = at;
                    put(0);
                    block = 0;
                }
            }
        };
        emit(256);
        for (u32 i = 0; i < fw * fh; ++i) {
            if (since == 254) {
                emit(256);
                since = 0;
            }
            emit(lcg(&seed) & 0xFF);
            ++since;
        }
        emit(257);
        if (bits) {
            put(static_cast<u32>(acc & 0xFF));
            ++block;
        }
        if (block_at < capacity)
            out[block_at] = static_cast<u8>(block);
        if (block)
            put(0);
    }
    put(0x3B);
    return at <= capacity ? at : 0;
}

void test_gif_crop() noexcept {
    Memory encoded(4096), scratch(4096);
    GifFixtureResult fixture{};
    if (!build_indexed_gif_fixture({encoded.data, encoded.bytes}, {scratch.data, scratch.bytes}, &fixture)) {
        check(false, "GIF fixture builds");
        return;
    }
    const ConstSlice<u8> source{encoded.data, fixture.encoded_bytes};
    const ProfileImageInfo probe = profile_image_probe(source);
    check(probe.error == ProfileImageError::none && probe.kind == GuildImageKind::gif && probe.width == 4 &&
              probe.height == 4 && probe.frames == 4 && probe.animated && probe.output_kind == GuildImageKind::gif,
          "animated GIF probe");
    constexpr u16 rotations[4] = {0, 90, 180, 270};
    for (const u16 rotation : rotations) {
        // The last frame (3,3) misses the crop and becomes one transparent pixel.
        const ImageCropOutput crop = crop_rect(1, 0, 3, 3, rotation, 1, 1);
        GuildImageBuffer out{};
        const ProfileImageInfo info = profile_image_crop(source, crop, &out);
        check(info.error == ProfileImageError::none && info.output_kind == GuildImageKind::gif && out.data &&
                  info.bytes == out.bytes && info.output_width == 3 && info.output_height == 3 &&
                  gif_crop_matches(source, {out.data, out.bytes}, crop),
              rotation == 0    ? "an animated GIF crop keeps every frame"
              : rotation == 90  ? "an animated GIF crop rotates 90 degrees"
              : rotation == 180 ? "an animated GIF crop rotates 180 degrees"
                                : "an animated GIF crop rotates 270 degrees");
        guild_image_release(&out);
    }
    // Large random frames drive the LZW encoder through every code width and clears.
    Memory big(64 * 1024);
    const u32 bytes = build_random_gif(big.data, big.bytes, 96, 80, 2, 11);
    const ConstSlice<u8> noisy{big.data, bytes};
    const ImageCropOutput crop = crop_rect(5, 3, 90, 70, 90);
    GuildImageBuffer out{};
    const ProfileImageInfo info = profile_image_crop(noisy, crop, &out);
    check(bytes && info.error == ProfileImageError::none && info.output_kind == GuildImageKind::gif &&
              info.output_width == 70 && info.output_height == 90 && gif_crop_matches(noisy, {out.data, out.bytes}, crop),
          "random GIF frames re-encode through full LZW tables");
    guild_image_release(&out);

    // A single-frame GIF is a still.
    Memory still(16 * 1024), lzw(16 * 1024), rgba(128 * 64 * 4);
    if (build_deferred_clear_gif_fixture({still.data, still.bytes}, {lzw.data, lzw.bytes}, &fixture)) {
        const ConstSlice<u8> single{still.data, fixture.encoded_bytes};
        GifFrames frames;
        bool exact = frames.parse(single);
        ProfileImageInfo s{};
        exact = exact && crop_png(single, crop_rect(10, 4, 20, 8, 0), &rgba, &s) &&
                s.output_kind == GuildImageKind::png && !s.animated;
        Memory indices(128 * 64);
        const auto decoded = gif_decode_parsed_frame(single, frames.image, 0, frames.workspace, {indices.data, indices.bytes});
        for (u32 y = 0; exact && y < 8; ++y)
            for (u32 x = 0; x < 20; ++x) {
                u32 want = decoded.palette.rgba[indices.data[(4 + y) * 128 + 10 + x]];
                if (!(want >> 24))
                    want = 0;
                exact = exact && decoded.ok() && pixel_at(rgba.data, 20, x, y) == want;
            }
        check(exact, "a single-frame GIF crops to a PNG");
    } else {
        check(false, "still GIF fixture builds");
    }
}

void test_invalid() noexcept {
    u8 pixels[4 * 4 * 4]{};
    for (u32 i = 0; i < 16; ++i)
        pixels[i * 4 + 3] = 255;
    Memory png(png_encode_bound(4, 4));
    const u32 bytes = encode_png(pixels, 4, 4, &png);
    const ConstSlice<u8> source{png.data, bytes};
    GuildImageBuffer out{};
    const auto rejects = [&](ConstSlice<u8> input, const ImageCropOutput& crop, ProfileImageError error) noexcept {
        out = {};
        const ProfileImageInfo info = profile_image_crop(input, crop, &out);
        return info.error == error && !out.data && !out.bytes && !info.bytes;
    };
    check(profile_image_crop(source, crop_rect(0, 0, 4, 4, 0), nullptr).error == ProfileImageError::input,
          "a missing output buffer is rejected");
    check(rejects({nullptr, 0}, crop_rect(0, 0, 1, 1, 0), ProfileImageError::input), "an empty source is rejected");
    const u8 text[] = "not an image at all";
    check(rejects({text, sizeof(text)}, crop_rect(0, 0, 1, 1, 0), ProfileImageError::format) &&
              profile_image_probe({text, sizeof(text)}).error == ProfileImageError::format,
          "unknown content is a format error");
    check(rejects(source, crop_rect(1, 0, 4, 4, 0), ProfileImageError::input), "a crop past the right edge is rejected");
    check(rejects(source, crop_rect(0, 3, 4, 2, 0), ProfileImageError::input), "a crop past the bottom is rejected");
    check(rejects(source, crop_rect(0, 0, 0, 4, 0), ProfileImageError::input), "an empty crop is rejected");
    check(rejects(source, crop_rect(0, 0, 4, 4, 45), ProfileImageError::input), "odd rotations are rejected");
    ImageCropOutput zero = crop_rect(0, 0, 4, 4, 0);
    zero.output_width = 0;
    check(rejects(source, zero, ProfileImageError::input), "an empty output size is rejected");
    zero.output_width = kProfileImageOutputDimensionMaximum + 1;
    check(rejects(source, zero, ProfileImageError::input), "an oversized output is rejected");
    check(rejects({png.data, bytes - 20}, crop_rect(0, 0, 4, 4, 0), ProfileImageError::decode) &&
              profile_image_probe({png.data, bytes - 20}).error == ProfileImageError::decode,
          "a truncated PNG is a decode error");
    Memory huge(kProfileImageSourceMaximumBytes + 1);
    copy_bytes(huge.data, png.data, bytes);
    check(rejects({huge.data, huge.bytes}, crop_rect(0, 0, 4, 4, 0), ProfileImageError::too_large) &&
              profile_image_probe({huge.data, huge.bytes}).error == ProfileImageError::too_large,
          "sources over 10 MB are too large");
    // A GIF whose second frame is corrupt falls back to a still of the first.
    Memory encoded(4096), scratch(4096);
    GifFixtureResult fixture{};
    if (build_indexed_gif_fixture({encoded.data, encoded.bytes}, {scratch.data, scratch.bytes}, &fixture)) {
        GifFrames frames;
        if (frames.parse({encoded.data, fixture.encoded_bytes}) && frames.image.frame_count > 1) {
            // Code 0 first is "missing clear" for the LZW decoder.
            encoded.data[frames.image.frames[1].data_offset + 1] = 0x00;
            Memory rgba(16 * 4);
            ProfileImageInfo info{};
            check(crop_png({encoded.data, fixture.encoded_bytes}, crop_rect(0, 0, 4, 4, 0), &rgba, &info) &&
                      info.output_kind == GuildImageKind::png && info.animated,
                  "a GIF the encoder cannot carry over becomes a still");
        }
    }
}

} // namespace profile_image_test
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_profile_image_entry() noexcept {
    using namespace maicord::profile_image_test;
    test_crop_layout();
    test_crop_interaction();
    test_zlib();
    test_png_encoder();
    test_png_crop();
    test_jpeg_crop();
    test_webp_crop();
    test_gif_crop();
    test_invalid();
    constexpr char pass[] = "PASS profile image crop\n";
    constexpr char fail[] = "FAIL profile image crop\n";
    write_text(failures ? fail : pass);
    ExitProcess(failures ? 1 : 0);
}
