// Profile media the CDN cannot size: effect layers have no `size` parameter and some ship at about twice the
// 450x880 effect slot (a 1048x2048 one failed to upload in issue #8 and restarted the session). The decode fits
// them to the slot, aspect kept, with an area-average filter:
//  - the filter against an independent double-precision area average, and a one-texel checkerboard that must
//    come out grey (no aliasing), not black and white;
//  - the media worker publishing a 1048x2048 still effect layer at 450x879 as RGBA8 PNG scanlines, another
//    profile image at the identity page's 1022 rows, and an animated (APNG) layer whose every frame lands on
//    the fitted canvas at its fitted offset.
#include "../../support/media_fixture.hpp"
#include "../../support/test_output.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/local_preview_thumbnail.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/core/encoded_cache.hpp"
#include "maicord/media/image/png_encode.hpp"
#include "maicord/render/media/atlas.hpp"

namespace maicord {
namespace profile_media_fit_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

constexpr u32 kWidth = 1048, kHeight = 2048;
constexpr u32 kFrameX = 101, kFrameY = 203, kFrameWidth = 300, kFrameHeight = 400;

// Fine detail in every channel and a varying, partly transparent alpha.
u32 detail(u32 x, u32 y) noexcept {
    const u32 r = (x * 7u + y * 3u) & 255u, g = (x ^ y) & 255u, b = (x + y) & 1u ? 255u : 0u;
    const u32 a = 64u + ((x * 5u + y * 11u) % 192u);
    return r | g << 8 | b << 16 | a << 24;
}
u32 checker(u32 x, u32 y) noexcept {
    return (x + y) & 1u ? 0xFFFFFFFFu : 0xFF000000u;
}
void fill(u8* rgba, u32 width, u32 height, u32 (*pixel)(u32, u32)) noexcept {
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const u32 value = pixel(x, y);
            copy_bytes(rgba + (static_cast<usize>(y) * width + x) * 4u, &value, 4);
        }
}

// The reference: each output texel the exact (double precision) area of the source it covers, colour
// weighted by alpha.
void reference_texel(const u8* rgba, u32 width, u32 height, const MediaFit& fit, u32 cx, u32 cy, double* out) noexcept {
    const double x0 = double(cx) * width / fit.width, x1 = double(cx + 1) * width / fit.width;
    const double y0 = double(cy) * height / fit.height, y1 = double(cy + 1) * height / fit.height;
    double sum[4]{};
    for (u32 y = static_cast<u32>(y0); y < height && double(y) < y1; ++y) {
        const double wy = (double(y + 1) < y1 ? double(y + 1) : y1) - (double(y) > y0 ? double(y) : y0);
        for (u32 x = static_cast<u32>(x0); x < width && double(x) < x1; ++x) {
            const double wx = (double(x + 1) < x1 ? double(x + 1) : x1) - (double(x) > x0 ? double(x) : x0);
            const u8* p = rgba + (static_cast<usize>(y) * width + x) * 4u;
            const double weight = wx * wy * p[3];
            sum[3] += weight;
            for (u32 c = 0; c < 3; ++c)
                sum[c] += weight * p[c];
        }
    }
    out[3] = sum[3] / ((x1 - x0) * (y1 - y0));
    for (u32 c = 0; c < 3; ++c)
        out[c] = sum[3] > 0 ? sum[c] / sum[3] : 0;
}
// Largest channel difference of a fitted image against the reference (colour only where alpha shows it).
u32 reference_error(const u8* source, const u8* fitted, u32 stride, u32 lead, const MediaFit& fit) noexcept {
    u32 worst = 0;
    for (u32 y = 0; y < fit.height; ++y)
        for (u32 x = 0; x < fit.width; ++x) {
            double expected[4];
            reference_texel(source, fit.source_width, fit.source_height, fit, x, y, expected);
            const u8* p = fitted + static_cast<usize>(y) * stride + lead + x * 4u;
            for (u32 c = 0; c < 4; ++c) {
                if (c < 3 && p[3] < 8)
                    continue;
                const double difference = p[c] > expected[c] ? p[c] - expected[c] : expected[c] - p[c];
                const u32 rounded = static_cast<u32>(difference + 0.5);
                worst = rounded > worst ? rounded : worst;
            }
        }
    return worst;
}

bool fit_whole(const u8* source, const MediaFit& fit, u8* output, u8* scratch, u32 scratch_bytes) noexcept {
    MediaFitSink sink{};
    if (!media_fit_begin(&sink, fit, {0, 0, fit.source_width, fit.source_height}, output, fit.width * 4u, 0,
                         {scratch, scratch_bytes}))
        return false;
    for (u32 y = 0; y < fit.source_height; ++y)
        if (!media_fit_push_row(&sink, source + static_cast<usize>(y) * fit.source_width * 4u))
            return false;
    return sink.cell_row == fit.height;
}

void test_filter(u8* source, u8* output, u8* scratch, u32 scratch_bytes) noexcept {
    MediaFit fit{};
    check(media_fit_plan(kWidth, kHeight, 450, 880, &fit) && fit.width == 450 && fit.height == 879,
          "fit: 1048x2048 fits the 450x880 effect slot at 450x879");
    MediaFit page{};
    check(media_fit_plan(kWidth, kHeight, kMediaAtlasExtent - 2u * kMediaAtlasPadding,
                         kMediaAtlasPageRows - 2u * kMediaAtlasPadding, &page) &&
              page.height == 1022 && page.width == 523,
          "fit: other profile images fit an identity page less its padding");
    MediaFit none{};
    check(!media_fit_plan(450, 880, 450, 880, &none) && !media_fit_plan(300, 600, 450, 880, &none),
          "fit: an image that fits its box is left alone");
    // Drawn as the renderer draws an effect (height = width * h / w): the aspect is the source's.
    const double drawn = 340.0 * fit.height / fit.width, source_aspect = 340.0 * kHeight / kWidth;
    check(drawn - source_aspect < 0.5 && source_aspect - drawn < 0.5, "fit: the fitted layer keeps its aspect");
    const MediaFitRegion region = media_fit_region(fit, kFrameX, kFrameY, kFrameWidth, kFrameHeight);
    check(region.x == 43 && region.y == 87 && region.x + region.width == 173 && region.y + region.height == 259,
          "fit: a frame region lands on the cells it covers");

    fill(source, kWidth, kHeight, detail);
    check(fit_whole(source, fit, output, scratch, scratch_bytes) &&
              reference_error(source, output, fit.width * 4u, 0, fit) <= 1,
          "fit: every texel matches the exact area average within one step");
    fill(source, kWidth, kHeight, checker);
    bool grey = fit_whole(source, fit, output, scratch, scratch_bytes);
    u64 total = 0;
    for (u32 i = 0; grey && i < fit.width * fit.height; ++i) {
        const u8 value = output[i * 4u];
        grey = output[i * 4u + 3] == 255 && value >= 96 && value <= 160;
        total += value;
    }
    const u64 mean = grey ? total / (fit.width * fit.height) : 0;
    check(grey && mean >= 125 && mean <= 130, "fit: one-texel detail averages to grey instead of aliasing");
}

// ---- The worker -------------------------------------------------------------------------------------------
MediaAssetKey effect_key(u64 id, bool animated, bool effect = true) noexcept {
    auto key = media_asset_embed(id, 1, effect ? kMediaProfileEffectLayerIndex : 0, MediaEmbedResource::image,
                                 MediaAttachmentFormat::png, animated, MediaResolutionTier::px512);
    key.kind = MediaAssetKind::profile_image;
    return key;
}

struct Encoder {
    u8* workspace;
    u64 workspace_bytes;
    u8* output;
    u64 output_bytes;
};
u32 encode(const Encoder& e, const u8* rgba, u32 width, u32 height, u8* output, u32 capacity) noexcept {
    return png_encode_rgba8(rgba, width, height, {e.workspace, static_cast<u32>(e.workspace_bytes)}, {output, capacity});
}
// The chunk of `type` in a PNG: its data and length.
const u8* find_chunk(const u8* png, u32 bytes, u32 type, u32* length) noexcept {
    for (u32 at = 8; at + 12 <= bytes;) {
        const u32 size = byte_load_be32(png + at);
        if (byte_load_be32(png + at + 4) == type) {
            *length = size;
            return png + at + 8;
        }
        at += 12 + size;
    }
    return nullptr;
}
void put_chunk(u8* out, u32* at, u32 type, const u8* data, u32 length) noexcept {
    png_fixture_write_be32(out + *at, length);
    png_fixture_write_be32(out + *at + 4, type);
    if (length)
        copy_bytes(out + *at + 8, data, length);
    png_fixture_write_be32(out + *at + 8 + length, png_fixture_crc32({out + *at + 4, length + 4}));
    *at += 12 + length;
}
void frame_control(u8* data, u32 sequence, u32 width, u32 height, u32 x, u32 y, u8 blend) noexcept {
    png_fixture_write_be32(data, sequence);
    png_fixture_write_be32(data + 4, width);
    png_fixture_write_be32(data + 8, height);
    png_fixture_write_be32(data + 12, x);
    png_fixture_write_be32(data + 16, y);
    data[20] = 0, data[21] = 1, data[22] = 0, data[23] = 10; // 1/10 s
    data[24] = 0;                                            // dispose none
    data[25] = blend;
}

bool request(AccountMedia* media, const MediaAssetKey& key, ConstSlice<u8> encoded, AccountMediaFrame* frame,
             u16* generation) noexcept {
    const auto stored = encoded_media_cache_store(&media->cache, key, encoded,
                                                  EncodedMediaCachePolicy{{reinterpret_cast<const u8*>("\"layer\""), 7}, 2'000, true, false});
    const AccountMediaAttachmentRequest request{
        utf8_literal("https://cdn.discordapp.com/assets/profile_effects/effects/fixture/layer.png"),
        key, 0, 0, 0, 0, CdnImageType::png, {0, 0, 0}, nullptr};
    if (generation)
        *generation = stored.generation;
    return stored.stored() &&
           account_media_request_attachment(media, request, 1'000, 1'100).state == AccountMediaRequestState::ready &&
           account_media_take_frame(media, frame);
}

bool rgba8_scanlines(const AccountMediaFrame& frame, u32 width, u32 height) noexcept {
    if (!frame.image || frame.image->width != width || frame.image->height != height ||
        frame.image->color_type != 6 || frame.image->bit_depth != 8 ||
        frame.bytes.count != (width * 4u + 1u) * height || frame.width != width || frame.height != height)
        return false;
    for (u32 y = 0; y < height; ++y)
        if (frame.bytes[y * (width * 4u + 1u)] != 0)
            return false;
    return true;
}

void test_worker(u8* source, const Encoder& encoder) noexcept {
    AccountMedia* media = commit_pages<AccountMedia>();
    if (!media || !account_media_initialize(media).ok()) {
        check(false, "worker: media lane initializes");
        return;
    }
    fill(source, kWidth, kHeight, detail);
    const u32 still = encode(encoder, source, kWidth, kHeight, encoder.output, static_cast<u32>(encoder.output_bytes));
    MediaFit fit{};
    media_fit_plan(kWidth, kHeight, 450, 880, &fit);

    AccountMediaFrame frame{};
    bool valid = still && request(media, effect_key(0xEF1, false), {encoder.output, still}, &frame, nullptr);
    check(valid && frame.kind == AccountMediaFrameKind::png_scanlines && rgba8_scanlines(frame, 450, 879),
          "worker: a 1048x2048 still effect layer is published at the 450x879 the effect slot takes");
    check(valid && reference_error(source, frame.bytes.data, 450u * 4u + 1u, 1, fit) <= 1,
          "worker: the published layer is the area average of the source");

    valid = still && request(media, effect_key(0xEF2, false, false), {encoder.output, still}, &frame, nullptr);
    check(valid && rgba8_scanlines(frame, 523, 1022),
          "worker: another oversized profile image is fitted to an identity page");

    // An APNG layer: the full canvas, then a 300x400 frame drawn over it at (101, 203).
    u8* apng = encoder.output + still;
    u32 at = 0;
    static constexpr u8 kSignature[8]{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    copy_bytes(apng, kSignature, 8);
    at = 8;
    u32 length = 0;
    const u8* header = find_chunk(encoder.output, still, 0x49484452u, &length);
    put_chunk(apng, &at, 0x49484452u, header, length);
    u8 control[26]{};
    png_fixture_write_be32(control, 2);
    png_fixture_write_be32(control + 4, 0);
    put_chunk(apng, &at, 0x6163544Cu, control, 8); // acTL
    frame_control(control, 0, kWidth, kHeight, 0, 0, 0);
    put_chunk(apng, &at, 0x6663544Cu, control, 26); // fcTL
    const u8* first = find_chunk(encoder.output, still, 0x49444154u, &length);
    put_chunk(apng, &at, 0x49444154u, first, length);
    // The second frame's pixels: the detail image's top-left 300x400, encoded alone.
    u8* second = apng + at + (32u << 20);
    u8* frame_rgba = source + static_cast<usize>(kWidth) * kHeight * 4u - kFrameWidth * kFrameHeight * 4u;
    for (u32 y = 0; y < kFrameHeight; ++y)
        copy_bytes(frame_rgba + y * kFrameWidth * 4u, source + static_cast<usize>(y) * kWidth * 4u, kFrameWidth * 4u);
    const u32 second_bytes = encode(encoder, frame_rgba, kFrameWidth, kFrameHeight, second, 4u << 20);
    frame_control(control, 1, kFrameWidth, kFrameHeight, kFrameX, kFrameY, 1);
    put_chunk(apng, &at, 0x6663544Cu, control, 26);
    const u8* data = find_chunk(second, second_bytes, 0x49444154u, &length);
    u8* fdat = second + second_bytes;
    png_fixture_write_be32(fdat, 2);
    if (data)
        copy_bytes(fdat + 4, data, length);
    put_chunk(apng, &at, 0x66644154u, fdat, length + 4); // fdAT
    put_chunk(apng, &at, 0x49454E44u, nullptr, 0);

    const MediaAssetKey animated = effect_key(0xEF3, true);
    u16 generation = 0;
    valid = second_bytes && data && request(media, animated, {apng, at}, &frame, &generation);
    check(valid && frame.kind == AccountMediaFrameKind::apng_scanlines && frame.frame_count == 2 &&
              frame.canvas_width == 450 && frame.canvas_height == 879 && rgba8_scanlines(frame, 450, 879) &&
              frame.apng_frame.x_offset == 0 && frame.apng_frame.y_offset == 0 && frame.apng_frame.width == 450 &&
              frame.apng_frame.height == 879,
          "worker: an animated 1048x2048 layer's first frame fills the fitted 450x879 canvas");
    const MediaFitRegion region = media_fit_region(fit, kFrameX, kFrameY, kFrameWidth, kFrameHeight);
    valid = valid && account_media_advance_animation(media, animated, generation, 1, 1'101).ok() &&
            account_media_take_frame(media, &frame);
    check(valid && frame.frame_index == 1 && frame.canvas_width == 450 && frame.canvas_height == 879 &&
              rgba8_scanlines(frame, region.width, region.height) && frame.apng_frame.x_offset == region.x &&
              frame.apng_frame.y_offset == region.y && frame.apng_frame.width == region.width &&
              frame.apng_frame.blend == ApngBlend::over,
          "worker: a later frame lands on the fitted canvas at its fitted offset, blend kept");
    check(account_media_stop(media, 5'000), "worker: media lane stops");
    pages_release(media, sizeof(AccountMedia));
}

} // namespace profile_media_fit_test
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_profile_media_fit_entry) {
    using namespace maicord;
    using namespace maicord::profile_media_fit_test;
    const u64 source_bytes = u64{kWidth} * kHeight * 4u;
    const u64 scratch_bytes = media_fit_scratch_bytes(kWidth, 1024);
    const u64 workspace_bytes = png_encode_workspace_bytes(kWidth, kHeight);
    const u64 output_bytes = 3u * png_encode_bound(kWidth, kHeight) + (40u << 20);
    u8* source = static_cast<u8*>(commit_pages(source_bytes));
    u8* fitted = static_cast<u8*>(commit_pages(source_bytes));
    u8* scratch = static_cast<u8*>(commit_pages(scratch_bytes));
    u8* workspace = static_cast<u8*>(commit_pages(workspace_bytes));
    u8* output = static_cast<u8*>(commit_pages(output_bytes));
    if (!source || !fitted || !scratch || !workspace || !output) {
        test_write_text("profile media fit: allocation failed\n");
        test_process_exit(2);
    }
    test_filter(source, fitted, scratch, static_cast<u32>(scratch_bytes));
    test_worker(source, {workspace, workspace_bytes, output, output_bytes});
    test_write_text(failures ? "profile media fit: FAILED\n" : "profile media fit: passed\n");
    test_process_exit(failures ? 1 : 0);
}
