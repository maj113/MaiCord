// GPU-assisted image decode against independent references. The CPU decoders
// hand the renderer intermediate data (JPEG coefficient blocks; VP8 modes,
// dequantized residuals and frame header), and the GPU finishes the picture.
// A decoder change that breaks that data still passes CPU-only output checks,
// so here the production decoders feed the production GPU stages and the
// pictures are compared with:
//   - PNG: generated scanlines across every admitted sample layout, with
//     integer expansion and premultiplication references;
//   - JPEG: Pillow/libjpeg-turbo decodes of the same files (full images and
//     the corpus sample grid), within the renderer's documented tolerance;
//   - VP8: the CPU WebP thumbnail decoder (its own prediction, loop filter and
//     libwebp-style upsampling), which must agree byte for byte, and libwebp's
//     own decode of the multi-macroblock fixture.
#include "fixtures/image_decode_corpus.hpp"
#include "jpeg_cmyk_fixture.hpp"
#include "jpeg_progressive_fixture.hpp"
#include "jpeg_reference_fixture.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/media/jpeg.hpp"
#include "maicord/media/lossy_webp.hpp"
#include "maicord/media/webp_thumbnail.hpp"
#include "maicord/render/media_atlas.hpp"
#include "maicord/render/media_atlas_gpu.hpp"
#include "../src/render/media_atlas_gpu_internal.hpp"
#include "webp_fixture.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

inline constexpr u16 kAtlasExtent = 512;
// The renderer's tolerance against Pillow (float IDCT and triangle chroma
// filter versus libjpeg-turbo's integer IDCT and fancy upsampling).
inline constexpr u32 kJpegTolerance = 2;

// VirtualAlloc'd: the decode workspace leads so it stays page (16-byte) aligned.
struct State {
    u8 workspace[4u << 20];
    JpegWorkspace jpeg;
    LossyWebpWorkspace lossy;
    Vp8lWorkspace lossless;
    MediaAtlas atlas;
    MediaAtlasEntry entries[16];
    u32 buckets[32];
    GlyphAtlasEntry allocations[16];
    GlyphAtlasRow rows[16];
    MediaAtlasGpu gpu;
    u8 surface[kAtlasExtent * kAtlasExtent * 4];
    u8 reference[kWebpDimensionMaximum * kWebpDimensionMaximum * 4];
};
State* g;
ID3D11Device* g_device;
ID3D11DeviceContext1* g_context;
ID3D11Texture2D* g_staging;
u32 g_failures;

void say(const char* text) noexcept {
    u32 length = 0;
    while (text[length]) {
        ++length;
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, nullptr);
}

void say_number(u64 value) noexcept {
    char digits[24];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    char text[24];
    for (u32 i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = 0;
    say(text);
}

void check(bool passed, const char* name, u64 detail = 0) noexcept {
    if (passed) {
        return;
    }
    ++g_failures;
    say("FAIL ");
    say(name);
    say(" ");
    say_number(detail);
    say("\r\n");
}

// Copies the allocation's RGBA8 texels from the atlas texture.
bool read_surface(const MediaAtlasResult& allocation) noexcept {
    g_context->CopyResource(g_staging, g->gpu.texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const u32 width = allocation.width, height = allocation.height;
    for (u32 y = 0; y < height; ++y) {
        const u8* row = static_cast<const u8*>(mapped.pData) + (allocation.y + y) * mapped.RowPitch + allocation.x * 4u;
        for (u32 x = 0; x < width * 4u; ++x) {
            g->surface[y * width * 4u + x] = row[x];
        }
    }
    g_context->Unmap(g_staging, 0);
    return true;
}

u32 channel_error(u8 actual, u8 expected) noexcept {
    return actual > expected ? actual - expected : expected - actual;
}

// All admitted PNG sample layouts, with a complete 16-bit sample sweep and
// every 8-bit color/alpha pair. Generate scanlines directly to isolate expansion.
void check_png_formats() noexcept {
    constexpr u8 depths[]{1, 2, 4, 8, 16}, types[]{0, 2, 3, 4, 6};
    u32 formats = 0;
    for (u8 type : types)
        for (u8 depth : depths) {
            PngImage image{};
            const u32 height = depth == 16 || (depth == 8 && type == 6) ? 256 : 1;
            if (!png_image_configure(&image, 256, height, depth, type))
                continue;
            ++formats;
            const u32 mask = (1u << depth) - 1;
            image.transparent_gray = mask / 2;
            image.transparent_red = 0;
            image.transparent_green = 19 & mask;
            image.transparent_blue = 38 & mask;
            image.has_transparency = true;
            if (type == 3) {
                image.palette_count = static_cast<u16>(mask + 1);
                for (u32 i = 0; i <= mask; ++i)
                    image.palette_rgba[i] =
                        i | ((255 - i) << 8) | ((i ^ 85) << 16) | (((i * 17) & 255) << 24);
            }
            clear_bytes(g->workspace, image.inflated_bytes);
            for (u32 y = 0; y < height; ++y)
                for (u32 x = 0; x < 256; ++x) {
                    const u32 pixel = y * 256 + x;
                    u32 samples[4]{}, expanded[4]{};
                    auto* row = g->workspace + y * image.row_stride + 1;
                    for (u32 c = 0; c < image.channels; ++c) {
                        const u32 value =
                            depth == 8 && type == 6 && c == 3 ? y : (pixel * 37 + c * 19) & mask;
                        samples[c] = value;
                        expanded[c] = depth == 16 ? (value * 255 + 32767) / 65535 : value * 255 / mask;
                        const u32 bit = (x * image.channels + c) * depth;
                        if (depth < 8)
                            row[bit / 8] |= static_cast<u8>(value << (8 - depth - bit % 8));
                        else if (depth == 8)
                            row[bit / 8] = static_cast<u8>(value);
                        else {
                            row[bit / 8] = static_cast<u8>(value >> 8);
                            row[bit / 8 + 1] = static_cast<u8>(value);
                        }
                    }
                    u32 rgba[]{expanded[0], expanded[1], expanded[2], 255};
                    if (type == 0 || type == 4) {
                        rgba[1] = rgba[2] = rgba[0];
                        rgba[3] = type == 4 ? expanded[1] : samples[0] == image.transparent_gray ? 0 : 255;
                    } else if (type == 2)
                        rgba[3] = samples[0] == image.transparent_red &&
                                          samples[1] == image.transparent_green &&
                                          samples[2] == image.transparent_blue
                                      ? 0
                                      : 255;
                    else if (type == 3) {
                        const u32 packed = image.palette_rgba[samples[0]];
                        for (u32 c = 0; c < 4; ++c)
                            rgba[c] = (packed >> (c * 8)) & 255;
                    } else
                        rgba[3] = expanded[3];
                    for (u32 c = 0; c < 4; ++c)
                        g->reference[pixel * 4 + c] =
                            static_cast<u8>(c == 3 ? rgba[c] : (rgba[c] * rgba[3] + 127) / 255);
                }
            media_atlas_reset(&g->atlas);
            const auto uploaded = media_atlas_gpu_upload_png(&g->gpu, &g->atlas, g_device, g_context,
                                                             media_asset_custom_emoji(9001, false), image,
                                                             {g->workspace, image.inflated_bytes});
            check(uploaded.ok() && read_surface(uploaded.allocation) &&
                      bytes_equal(g->surface, g->reference, 256 * height * 4),
                  "png_sample_expansion", type * 100 + depth);
        }
    check(formats == 15, "png_format_coverage", formats);
}

// ---- JPEG ------------------------------------------------------------------------------
bool upload_jpeg(ConstSlice<u8> encoded, u64 id, JpegImage* image, MediaAtlasResult* allocation,
                 bool damage = false) noexcept {
    media_atlas_reset(&g->atlas);
    if (!jpeg_decode_coefficients(encoded, image, &g->jpeg).ok()) {
        return false;
    }
    if (damage) {
        g->jpeg.coefficients.blocks[0].values[0] = static_cast<i16>(g->jpeg.coefficients.blocks[0].values[0] + 32);
    }
    const MediaAtlasGpuResult uploaded = media_atlas_gpu_upload_jpeg(
        &g->gpu, &g->atlas, g_device, g_context, media_asset_custom_emoji(id, false), *image, g->jpeg.coefficients);
    *allocation = uploaded.allocation;
    return uploaded.ok() && allocation->width == jpeg_display_width(*image) &&
           allocation->height == jpeg_display_height(*image) && read_surface(*allocation);
}

// Whole picture against a Pillow RGB decode.
// damage: one altered DC coefficient must push the picture past the tolerance.
void check_jpeg_reference(const char* name, ConstSlice<u8> encoded, ConstSlice<u8> rgb, u64 id,
                          bool damage = false) noexcept {
    JpegImage image{};
    MediaAtlasResult allocation{};
    if (!upload_jpeg(encoded, id, &image, &allocation, damage) ||
        rgb.count != u32{allocation.width} * allocation.height * 3u) {
        check(false, name, 0);
        return;
    }
    u32 largest = 0;
    bool opaque = true;
    for (u32 pixel = 0; pixel < u32{allocation.width} * allocation.height; ++pixel) {
        for (u32 channel = 0; channel < 3; ++channel) {
            largest = maximum(largest, channel_error(g->surface[pixel * 4 + channel], rgb[pixel * 3 + channel]));
        }
        opaque = opaque && g->surface[pixel * 4 + 3] == 255;
    }
    check(opaque && (damage ? largest > kJpegTolerance : largest <= kJpegTolerance), name, largest);
}

// The generated corpus: every sampling layout, restart intervals, progressive
// and grayscale, against Pillow's decode on a 4x4 grid.
void check_jpeg_corpus() noexcept {
    u32 images = 0;
    for (const ImageCorpusEntry& entry : kImageCorpus) {
        if (entry.kind != ImageCorpusKind::jpeg) {
            continue;
        }
        JpegImage image{};
        MediaAtlasResult allocation{};
        if (!upload_jpeg({entry.data, entry.bytes}, 2000 + images, &image, &allocation) ||
            image.orientation != JpegOrientation::top_left) {
            check(false, entry.name, 0);
            continue;
        }
        ++images;
        u32 largest = 0;
        for (u32 j = 0; j < 4; ++j) {
            for (u32 i = 0; i < 4; ++i) {
                const u32 x = (entry.width - 1) * i / 3, y = (entry.height - 1) * j / 3;
                const u8* actual = g->surface + (y * u32{allocation.width} + x) * 4u;
                for (u32 channel = 0; channel < 4; ++channel) {
                    largest = maximum(largest, channel_error(actual[channel], entry.samples[j * 4 + i][channel]));
                }
            }
        }
        check(largest <= kJpegTolerance, entry.name, largest);
    }
    check(images >= 10, "jpeg_corpus_images", images);
}

// ---- VP8 -------------------------------------------------------------------------------
struct Rows {
    u8* rgba;
    u32 width;
};

bool store_row(void* context, u32 y, const u8* rgba) noexcept {
    const auto& rows = *static_cast<const Rows*>(context);
    for (u32 x = 0; x < rows.width * 4u; ++x) {
        rows.rgba[y * rows.width * 4u + x] = rgba[x];
    }
    return true;
}

// expected_hash, when nonzero, is libwebp's RGBA FNV-1a for the file: an oracle
// independent of the token and mode parsing both MaiCord paths share.
void check_vp8(const char* name, ConstSlice<u8> encoded, u64 id, u64 expected_hash = 0,
               bool damage = false) noexcept {
    media_atlas_reset(&g->atlas);
    WebpImage image{};
    Vp8Keyframe frame{};
    Vp8ControlHeader header{};
    LossyWebpWorkspace& lossy = g->lossy;
    if (!lossy_webp_decode_keyframe(encoded, &image, &frame, &header, &lossy.probabilities, &lossy.modes,
                                    &lossy.coefficients, nullptr, {}, {})
             .ok() ||
        image.has_alpha) {
        check(false, name, 1);
        return;
    }
    if (damage) {
        Vp8MacroblockCoefficients& first = lossy.coefficients.macroblocks[0];
        first.values[0][0] = static_cast<i16>(first.values[0][0] + 256);
        first.has_coefficients = 1;
    }
    MediaAtlasGpuResult result =
        media_atlas_gpu_reconstruct_vp8_keyframe(&g->gpu, g_device, g_context, frame, lossy.modes, lossy.coefficients);
    if (result.ok()) {
        result = media_atlas_gpu_filter_vp8_keyframe(&g->gpu, g_device, g_context, frame, header, lossy.modes,
                                                     lossy.coefficients);
    }
    if (result.ok()) {
        result = media_atlas_gpu_upload_vp8_keyframe(&g->gpu, &g->atlas, g_device, g_context,
                                                     media_asset_custom_emoji(id, false), frame, {});
    }
    if (!result.ok() || result.allocation.width != frame.width || result.allocation.height != frame.height ||
        !read_surface(result.allocation)) {
        check(false, name, 2);
        return;
    }
    WebpThumbnailLayout layout{};
    Rows rows{g->reference, frame.width};
    if (webp_thumbnail_parse(encoded, &layout) != WebpThumbnailError::none ||
        layout.workspace_bytes > sizeof(g->workspace) || layout.width != frame.width ||
        webp_thumbnail_decode(layout, {g->workspace, layout.workspace_bytes}, store_row, &rows) !=
            WebpThumbnailError::none) {
        check(false, name, 3);
        return;
    }
    u32 different = 0;
    u64 hash = 1'469'598'103'934'665'603ull;
    for (u32 index = 0; index < u32{frame.width} * frame.height * 4u; ++index) {
        different += g->surface[index] != g->reference[index] ? 1u : 0u;
        hash = (hash ^ g->surface[index]) * 1'099'511'628'211ull;
    }
    if (damage) {
        check(different != 0, name, different);
        return;
    }
    check(different == 0, name, different);
    check(expected_hash == 0 || hash == expected_hash, name, hash);
}

// A large image's dynamic upload buffer (CPU-visible memory) is replaced by
// the next small upload instead of staying mapped.
void check_upload_shrink() noexcept {
    using media_atlas_gpu_detail::ensure_dynamic_raw_buffer;
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    u32 capacity = 0;
    const bool grown = SUCCEEDED(ensure_dynamic_raw_buffer(g_device, 16u << 20, &buffer, &view, &capacity)) &&
                       capacity == 16u << 20;
    const bool kept = SUCCEEDED(ensure_dynamic_raw_buffer(g_device, 8u << 20, &buffer, &view, &capacity)) &&
                      capacity == 16u << 20;
    const bool shrunk = SUCCEEDED(ensure_dynamic_raw_buffer(g_device, 64u << 10, &buffer, &view, &capacity)) &&
                        capacity == 64u << 10;
    const bool small = SUCCEEDED(ensure_dynamic_raw_buffer(g_device, 4096, &buffer, &view, &capacity)) &&
                       capacity == 64u << 10 && buffer && view;
    if (view)
        view->Release();
    if (buffer)
        buffer->Release();
    check(grown && kept && shrunk && small, "dynamic_upload_shrinks_after_large_image", capacity);
}

int run() noexcept {
    g = static_cast<State*>(VirtualAlloc(nullptr, sizeof(State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g) {
        return 2;
    }
    // A hardware device when there is one; WARP runs the same shaders anywhere.
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11DeviceContext* context = nullptr;
    HRESULT status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                       &g_device, nullptr, &context);
    if (FAILED(status)) {
        status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device,
                                   nullptr, &context);
    }
    if (FAILED(status) ||
        FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_context)))) {
        // WARP ships with Windows, so no device at all is a failure, not a skip.
        say("media gpu reference: no D3D11 device\r\n");
        return 5;
    }
    context->Release();
    D3D11_TEXTURE2D_DESC description{};
    if (!media_atlas_initialize(&g->atlas, kAtlasExtent, kAtlasExtent, 1, {g->entries, 16}, {g->buckets, 32},
                                {g->allocations, 16}, {g->rows, 16}) ||
        !media_atlas_gpu_initialize(&g->gpu, g_device, g_context, kAtlasExtent, kAtlasExtent).ok()) {
        say("media gpu reference: atlas initialization failed\r\n");
        return 3;
    }
    g->gpu.texture->GetDesc(&description);
    description.BindFlags = 0;
    description.MiscFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_device->CreateTexture2D(&description, nullptr, &g_staging)) ||
        description.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        say("media gpu reference: staging texture failed\r\n");
        return 4;
    }

    check_png_formats();
    check_jpeg_reference("jpeg_420_reference", {kJpeg420Reference, sizeof(kJpeg420Reference)},
                         {kJpeg420ReferenceRgb, sizeof(kJpeg420ReferenceRgb)}, 1001);
    check_jpeg_reference("jpeg_progressive_reference",
                         {reinterpret_cast<const u8*>(kJpegProgressive420Reference), kJpegProgressive420ReferenceBytes},
                         {reinterpret_cast<const u8*>(kJpegProgressive420ReferenceRgb),
                          kJpegProgressive420ReferenceRgbBytes},
                         1002);
    check_jpeg_reference("jpeg_cmyk_reference", {kJpegCmykAdobeReference, sizeof(kJpegCmykAdobeReference)},
                         {kJpegCmykAdobeReferenceRgb, sizeof(kJpegCmykAdobeReferenceRgb)}, 1003);
    check_jpeg_reference("jpeg_ycck_progressive_reference",
                         {kJpegYcckProgressiveReference, sizeof(kJpegYcckProgressiveReference)},
                         {kJpegYcckProgressiveReferenceRgb, sizeof(kJpegYcckProgressiveReferenceRgb)}, 1004);
    check_jpeg_corpus();
    check_jpeg_reference("jpeg_damage_detected", {kJpeg420Reference, sizeof(kJpeg420Reference)},
                         {kJpeg420ReferenceRgb, sizeof(kJpeg420ReferenceRgb)}, 1005, true);

    check_vp8("vp8_4x4", {kWebpLossy4x4, sizeof(kWebpLossy4x4)}, 3001);
    check_vp8("vp8_32x24", {kWebpLossy32x24, sizeof(kWebpLossy32x24)}, 3002, kWebpLossy32x24RgbaFnv1a);
    u32 lossy = 0;
    for (const ImageCorpusEntry& entry : kImageCorpus) {
        WebpImage image{};
        if (entry.kind == ImageCorpusKind::webp_lossy && webp_parse({entry.data, entry.bytes}, &image).ok() &&
            !image.has_alpha) {
            check_vp8(entry.name, {entry.data, entry.bytes}, 3100 + lossy++);
        }
    }
    check(lossy >= 1, "vp8_corpus_images", lossy);
    check_vp8("vp8_damage_detected", {kWebpLossy32x24, sizeof(kWebpLossy32x24)}, 3003, 0, true);
    check_upload_shrink();

    media_atlas_gpu_release(&g->gpu);
    g_staging->Release();
    g_context->Release();
    g_device->Release();
    return g_failures ? 1 : 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_media_gpu_reference_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
