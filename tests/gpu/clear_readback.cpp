// The GPU API on the build's backend without shaders: a device, textures of every format, render-target clears,
// the top-left orientation of uploads and readbacks, region copies, buffers and the status of refused requests.
// Linux runs it on whatever EGL finds (Mesa llvmpipe by default; GALLIUM_DRIVER=d3d12 selects the WSL GPU).
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "../support/test_output.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;
u32 g_checks = 0;

void check(bool condition, const char* label) noexcept {
    ++g_checks;
    test_check(&g_failures, condition, label);
}

struct Rgba {
    u8 r, g, b, a;
};

bool close(u8 value, u8 expected, u32 tolerance) noexcept {
    return value + tolerance >= expected && value <= expected + tolerance;
}

bool pixel_is(const Rgba& pixel, Rgba expected, u32 tolerance = 1) noexcept {
    return close(pixel.r, expected.r, tolerance) && close(pixel.g, expected.g, tolerance) &&
           close(pixel.b, expected.b, tolerance) && close(pixel.a, expected.a, tolerance);
}

constexpr u32 kWidth = 64, kHeight = 48;
Rgba g_pixels[kWidth * kHeight];
Rgba g_other[kWidth * kHeight];

GpuTexture* make_target(GpuDevice* device, GpuFormat format, u32 width, u32 height, GpuRtv** view) noexcept {
    GpuTexture* texture = nullptr;
    const GpuTextureDesc desc{width, height, format, GpuUsage::default_,
                              static_cast<u8>(gpu_bind_srv | gpu_bind_rtv)};
    if (!gpu_ok(gpu_texture_create(device, desc, nullptr, &texture)))
        return nullptr;
    if (!gpu_ok(gpu_rtv_create(device, texture, view))) {
        gpu_release(texture);
        return nullptr;
    }
    return texture;
}

void test_clear_and_rectangles(GpuDevice* device, GpuContext* context) noexcept {
    GpuRtv* view = nullptr;
    GpuTexture* texture = make_target(device, GpuFormat::rgba8, kWidth, kHeight, &view);
    check(texture && view, "rgba8 render target");
    if (!texture)
        return;
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    gpu_clear_rtv(context, view, color);
    check(gpu_ok(gpu_readback(device, context, texture, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4)),
          "readback of the whole target");
    bool uniform = true;
    for (const Rgba& pixel : g_pixels)
        uniform = uniform && pixel_is(pixel, {64, 128, 191, 255});
    check(uniform, "clear fills every pixel");

    // A rectangle near the top: row 4 is its first row, so the origin is top-left in the readback too.
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    gpu_clear_rtv_rect(context, view, red, {8, 4, 24, 20});
    check(gpu_ok(gpu_readback(device, context, texture, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4)),
          "readback after a rectangle clear");
    bool inside = true, outside = true;
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x) {
            const bool in = x >= 8 && x < 24 && y >= 4 && y < 20;
            const Rgba& pixel = g_pixels[y * kWidth + x];
            if (in)
                inside = inside && pixel_is(pixel, {255, 0, 0, 255});
            else
                outside = outside && pixel_is(pixel, {64, 128, 191, 255});
        }
    check(inside, "the rectangle clear fills its rectangle");
    check(outside, "the rectangle clear leaves the rest");

    // A scissor left behind by drawing does not limit a clear.
    gpu_set_scissor(context, {0, 0, 1, 1});
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    gpu_clear_rtv(context, view, green);
    gpu_readback(device, context, texture, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4);
    check(pixel_is(g_pixels[(kHeight - 1) * kWidth + kWidth - 1], {0, 255, 0, 255}) && pixel_is(g_pixels[0], {0, 255, 0, 255}),
          "a full clear ignores the scissor");

    // Readback of a sub-box into a wider pitch.
    const u32 pitch = 40 * 4;
    static Rgba wide[40 * 8];
    clear_bytes(wide, sizeof(wide));
    check(gpu_ok(gpu_readback(device, context, texture, {10, 10, 20, 14}, wide, pitch)), "readback into a wide pitch");
    check(pixel_is(wide[0], {0, 255, 0, 255}) && pixel_is(wide[3 * 40 + 9], {0, 255, 0, 255}) &&
              wide[3 * 40 + 10].a == 0 && wide[4 * 40].a == 0,
          "a wide pitch writes only the box");

    GpuRtv* released_view = view;
    gpu_release(released_view);
    gpu_release(texture);
    check(!released_view && !texture, "release clears the handle");
}

void test_upload_orientation_and_copy(GpuDevice* device, GpuContext* context) noexcept {
    // Row y of the upload holds (y, x, 7, 255): a flipped or transposed path shows at once.
    static Rgba source[kWidth * kHeight];
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x)
            source[y * kWidth + x] = {static_cast<u8>(y), static_cast<u8>(x), 7, 255};
    GpuTexture* a = nullptr;
    const GpuInitialData data{source, kWidth * 4};
    check(gpu_ok(gpu_texture_create(device, {kWidth, kHeight, GpuFormat::rgba8, GpuUsage::immutable, gpu_bind_srv}, &data,
                                    &a)),
          "immutable texture with data");
    if (!a)
        return;
    check(gpu_ok(gpu_readback(device, context, a, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4)),
          "readback of an immutable texture");
    check(bytes_equal(g_pixels, source, sizeof(source)), "upload and readback share the row order");

    GpuRtv* view = nullptr;
    GpuTexture* b = make_target(device, GpuFormat::rgba8, kWidth, kHeight, &view);
    check(b != nullptr, "copy destination");
    if (b) {
        const float black[4] = {0, 0, 0, 0};
        gpu_clear_rtv(context, view, black);
        gpu_copy_region(context, b, 30, 20, a, {4, 2, 20, 10});
        check(gpu_ok(gpu_readback(device, context, b, {0, 0, kWidth, kHeight}, g_other, kWidth * 4)), "copy readback");
        bool copied = true, untouched = true;
        for (u32 y = 0; y < kHeight; ++y)
            for (u32 x = 0; x < kWidth; ++x) {
                const bool in = x >= 30 && x < 46 && y >= 20 && y < 28;
                const Rgba& pixel = g_other[y * kWidth + x];
                if (in)
                    copied = copied && pixel_is(pixel, source[(y - 18) * kWidth + (x - 26)], 0);
                else
                    untouched = untouched && pixel.a == 0;
            }
        check(copied, "copy moves the box to the destination");
        check(untouched, "copy touches nothing else");
    }
    GpuRtv* released_view = view;
    gpu_release(released_view);
    gpu_release(b);
    gpu_release(a);

    // BGRA bytes round-trip as stored (a sampled-only format).
    const Rgba bgra[4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
    const GpuInitialData bgra_data{bgra, 8};
    GpuTexture* c = nullptr;
    // A sampled bgra8 texture has no render path, so read back through a copy-compatible default texture.
    check(gpu_ok(gpu_texture_create(device, {2, 2, GpuFormat::bgra8, GpuUsage::immutable, gpu_bind_srv}, &bgra_data, &c)),
          "bgra8 sampled texture");
    gpu_release(c);
    GpuTexture* refused = nullptr;
    check(gpu_texture_create(device, {2, 2, GpuFormat::bgra8, GpuUsage::default_, gpu_bind_rtv}, nullptr, &refused) ==
              kGpuUnsupported && !refused,
          "bgra8 render targets are refused");
}

void test_single_and_dual_channel(GpuDevice* device, GpuContext* context) noexcept {
    static u8 plane[16 * 8];
    for (u32 y = 0; y < 8; ++y)
        for (u32 x = 0; x < 16; ++x)
            plane[y * 16 + x] = static_cast<u8>(y * 16 + x);
    static u8 back[16 * 8];
    const GpuInitialData data{plane, 16};
    const u8 binds[2] = {static_cast<u8>(gpu_bind_srv), static_cast<u8>(gpu_bind_srv | gpu_bind_uav)};
    for (const u8 bind : binds) {
        GpuTexture* texture = nullptr;
        check(gpu_ok(gpu_texture_create(device, {16, 8, GpuFormat::r8, GpuUsage::default_, bind}, &data, &texture)),
              "r8 texture");
        if (!texture)
            continue;
        clear_bytes(back, sizeof(back));
        check(gpu_ok(gpu_readback(device, context, texture, {0, 0, 16, 8}, back, 16)), "r8 readback");
        check(bytes_equal(back, plane, sizeof(plane)), (bind & gpu_bind_uav) ? "r8 as a storage image keeps its bytes"
                                                                              : "r8 keeps its bytes");
        gpu_release(texture);
    }
    static u8 pairs[8 * 4 * 2];
    for (u32 index = 0; index < sizeof(pairs); ++index)
        pairs[index] = static_cast<u8>(index * 3 + 1);
    static u8 pairs_back[sizeof(pairs)];
    GpuTexture* rg = nullptr;
    const GpuInitialData pair_data{pairs, 8 * 2};
    check(gpu_ok(gpu_texture_create(device, {8, 4, GpuFormat::rg8, GpuUsage::default_, gpu_bind_srv}, &pair_data, &rg)),
          "rg8 texture");
    if (rg) {
        check(gpu_ok(gpu_readback(device, context, rg, {0, 0, 8, 4}, pairs_back, 16)), "rg8 readback");
        check(bytes_equal(pairs, pairs_back, sizeof(pairs)), "rg8 keeps its bytes");
        gpu_release(rg);
    }
}

void test_float_targets(GpuDevice* device, GpuContext* context) noexcept {
    const GpuCaps& caps = gpu_caps(device);
    const float value[4] = {0.5f, 0.25f, 1.5f, 2.0f};
    if (caps.half_render_targets) {
        GpuRtv* view = nullptr;
        GpuTexture* texture = make_target(device, GpuFormat::rgba16f, 8, 8, &view);
        check(texture != nullptr, "rgba16f render target");
        if (texture) {
            gpu_clear_rtv(context, view, value);
            u16 halves[8 * 8 * 4];
            check(gpu_ok(gpu_readback(device, context, texture, {0, 0, 8, 8}, halves, 8 * 8)), "rgba16f readback");
            check(halves[0] == 0x3800 && halves[1] == 0x3400 && halves[2] == 0x3E00 && halves[3] == 0x4000 &&
                      halves[8 * 8 * 4 - 1] == 0x4000,
                  "rgba16f holds the cleared value");
            GpuRtv* released = view;
            gpu_release(released);
            gpu_release(texture);
        }
    }
    if (caps.float_render_targets) {
        GpuRtv* view = nullptr;
        GpuTexture* texture = make_target(device, GpuFormat::rgba32f, 8, 8, &view);
        check(texture != nullptr, "rgba32f render target");
        if (texture) {
            gpu_clear_rtv(context, view, value);
            float floats[8 * 8 * 4];
            check(gpu_ok(gpu_readback(device, context, texture, {0, 0, 8, 8}, floats, 8 * 16)), "rgba32f readback");
            check(floats[0] == 0.5f && floats[1] == 0.25f && floats[2] == 1.5f && floats[3] == 2.0f &&
                      floats[8 * 8 * 4 - 4] == 0.5f,
                  "rgba32f holds the cleared value");
            GpuRtv* released = view;
            gpu_release(released);
            gpu_release(texture);
        }
    }
}

void test_buffers_and_refusals(GpuDevice* device, GpuContext* context) noexcept {
    GpuBuffer* vertices = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {256, GpuUsage::dynamic, gpu_bind_vertex, false}, nullptr, &vertices)),
          "dynamic vertex buffer");
    if (vertices) {
        GpuStatus status = kGpuFail;
        void* mapped = gpu_map(context, vertices, GpuMap::discard, &status);
        check(mapped && gpu_ok(status), "map discard");
        if (mapped) {
            static_cast<u8*>(mapped)[255] = 1;
            gpu_unmap(context, vertices);
        }
        mapped = gpu_map(context, vertices, GpuMap::no_overwrite, &status);
        check(mapped && gpu_ok(status), "map no-overwrite");
        if (mapped)
            gpu_unmap(context, vertices);
        gpu_release(vertices);
    }
    GpuBuffer* constants = nullptr;
    const float values[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    check(gpu_ok(gpu_buffer_create(device, {32, GpuUsage::default_, gpu_bind_constant, false}, values, &constants)),
          "constant buffer");
    if (constants) {
        gpu_update_buffer(context, constants, values, sizeof(values));
        gpu_release(constants);
    }
    GpuBuffer* raw = nullptr;
    GpuSrv* raw_view = nullptr;
    GpuUav* raw_uav = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {1024, GpuUsage::default_, gpu_bind_srv | gpu_bind_uav, true}, nullptr, &raw)),
          "raw buffer");
    if (raw) {
        check(gpu_ok(gpu_srv_create(device, raw, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, 1024 / 4}, &raw_view)) &&
                  gpu_ok(gpu_uav_create(device, raw, 1024 / 4, &raw_uav)),
              "raw buffer views");
        gpu_release(raw_view);
        gpu_release(raw_uav);
        gpu_release(raw);
    }
    // The text-effect block of the renderer is 20496 bytes: a driver below that must refuse, not fail later.
    GpuBuffer* big = nullptr;
    const GpuStatus big_status =
        gpu_buffer_create(device, {20496, GpuUsage::dynamic, gpu_bind_constant, false}, nullptr, &big);
    check(gpu_caps(device).max_uniform_block_bytes >= 20496 ? gpu_ok(big_status) : big_status == kGpuUnsupported,
          "a constant buffer follows the uniform block limit");
    gpu_release(big);
    GpuTexture* refused = nullptr;
    check(gpu_texture_create(device, {0, 4, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv}, nullptr, &refused) ==
              kGpuInvalidArgument && !refused,
          "an empty texture is refused");
    check(gpu_texture_create(device, {4, 4, GpuFormat::r32_uint, GpuUsage::default_, gpu_bind_srv}, nullptr, &refused) ==
              kGpuUnsupported,
          "r32_uint is not a texture format");
    check(gpu_texture_create(device,
                             {gpu_caps(device).max_texture_size + 1, 4, GpuFormat::rgba8, GpuUsage::default_,
                              gpu_bind_srv},
                             nullptr, &refused) == kGpuInvalidArgument,
          "an oversize texture is refused");

    // The slabs recycle: far more create/release pairs than slots.
    bool recycled = true;
    for (u32 index = 0; index < 1000 && recycled; ++index) {
        GpuTexture* texture = nullptr;
        GpuBuffer* buffer = nullptr;
        recycled = gpu_ok(gpu_texture_create(device, {4, 4, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv}, nullptr,
                                             &texture)) &&
                   gpu_ok(gpu_buffer_create(device, {64, GpuUsage::default_, gpu_bind_constant, false}, nullptr, &buffer));
        gpu_release(texture);
        gpu_release(buffer);
    }
    check(recycled, "handles are recycled");
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    const GpuStatus status = gpu_device_create(&device, &context);
    check(gpu_ok(status) && device && context, "device creation");
    if (!gpu_ok(status))
        return;
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));
    const GpuCaps& caps = gpu_caps(device);
    test_write_metric("caps max_texture_size=", caps.max_texture_size);
    test_write_metric("caps max_uniform_block_bytes=", caps.max_uniform_block_bytes);
    test_write_metric("caps max_compute_invocations=", caps.max_compute_invocations);
    test_write_metric("caps max_shared_bytes=", caps.max_shared_bytes);
    test_write_metric("caps float_render_targets=", caps.float_render_targets);
    test_write_metric("caps half_render_targets=", caps.half_render_targets);
    test_write_metric("caps float_linear=", caps.float_linear);
    test_write_metric("caps timer_query=", caps.timer_query);
    check(caps.compute && caps.max_texture_size >= 2048 && caps.max_compute_invocations >= 128 &&
              caps.max_shared_bytes >= 16384 && caps.max_compute_storage_buffers >= 4,
          "caps meet the ES 3.1 floor");
    test_clear_and_rectangles(device, context);
    test_upload_orientation_and_copy(device, context);
    test_single_and_dual_channel(device, context);
    test_float_targets(device, context);
    test_buffers_and_refusals(device, context);
    check(!gpu_device_lost(device), "the device is not lost");
    gpu_device_destroy(&device, &context);
    check(!device && !context, "device destruction");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_clear_readback_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_write_metric("checks=", maicord::g_checks);
    maicord::test_exit(maicord::g_failures, "maicord_gpu_clear_readback");
}
