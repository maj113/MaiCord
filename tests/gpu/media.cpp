// Runs the media scene (media_scene.hpp) through the GPU API: the PNG, VP8 and JPEG compute shaders write synthetic
// pictures into an RGBA8 atlas (the over-blends read the atlas they write, through the second image binding the
// OpenGL ES translation declares), a GPU-written raw buffer carries the JPEG samples from one dispatch to the next,
// and the media shaders draw quads sampling the atlas, the animation cache and the preview atlas. Both results are
// compared with tests/gpu/golden/media_atlas.png and media_draw.png, which the Direct3D 11 side (media_windows.cpp)
// captured on Windows. MAICORD_GPU_MEDIA_DUMP=<prefix> also writes <prefix>.atlas and <prefix>.draw as raw RGBA
// bytes, for inspecting a difference.
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../support/test_output.hpp"
#include "media_scene.hpp"

namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kAtlasGolden[] = {
#embed "golden/media_atlas.png"
};
alignas(16) constexpr u8 kDrawGolden[] = {
#embed "golden/media_draw.png"
};
#pragma clang diagnostic pop

constexpr u32 kAtlasBytes = media_scene::kAtlasWidth * media_scene::kAtlasHeight * 4;
constexpr u32 kFrameBytes = media_scene::kFrameWidth * media_scene::kFrameHeight * 4;

u8 g_atlas[kAtlasBytes];
u8 g_frame[kFrameBytes];
u8 g_atlas_golden[kAtlasBytes];
u8 g_frame_golden[kFrameBytes];
media_scene::Data g_data;
u8 g_side[2][16 * 16 * 4];

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

void dump(const char* prefix, const char* suffix, const void* data, u32 bytes) noexcept {
    char path[512];
    u32 at = 0;
    for (const char* c = prefix; *c && at < 500; ++c)
        path[at++] = *c;
    for (const char* c = suffix; *c; ++c)
        path[at++] = *c;
    path[at] = 0;
    const long file = sys_open(path, kOpenWriteOnly | kOpenCreate | kOpenTruncate, 0644);
    if (file < 0)
        return;
    sys_write(static_cast<int>(file), data, bytes);
    sys_close(static_cast<int>(file));
}

constexpr u32 groups(u32 extent) noexcept {
    return (extent + 7) / 8;
}

// A raw byte-address buffer a shader reads: made, filled through a map (the production path), viewed.
struct RawInput {
    GpuBuffer* buffer;
    GpuSrv* view;
};
bool make_input(GpuDevice* device, GpuContext* context, const void* data, u32 bytes, RawInput* out) noexcept {
    if (!gpu_ok(gpu_buffer_create(device, {bytes, GpuUsage::dynamic, gpu_bind_srv, true}, nullptr, &out->buffer)) ||
        !gpu_ok(gpu_srv_create(device, out->buffer, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, bytes / 4}, &out->view)))
        return false;
    void* mapped = gpu_map(context, out->buffer, GpuMap::discard, nullptr);
    if (!mapped)
        return false;
    copy_bytes(mapped, data, bytes);
    gpu_unmap(context, out->buffer);
    return true;
}
void release_input(RawInput* input) noexcept {
    gpu_release(input->view);
    gpu_release(input->buffer);
}

struct Compute {
    GpuDevice* device;
    GpuContext* context;
    GpuUav* atlas;
};

// One dispatch: constants in b0, shader resources from t0, the atlas as u0 and (for the over-blends) u1.
bool dispatch(const Compute& compute, GpuComputeShader* shader, const void* constants, u32 constant_bytes,
              u32 view_count, GpuSrv* const* views, GpuUav* output, u32 groups_x, u32 groups_y) noexcept {
    GpuBuffer* buffer = nullptr;
    if (!gpu_ok(gpu_buffer_create(compute.device, {constant_bytes, GpuUsage::default_, gpu_bind_constant, false},
                                  constants, &buffer)))
        return false;
    GpuBuffer* constant_buffers[1] = {buffer};
    GpuUav* uavs[2] = {output, output};
    gpu_set_shader(compute.context, shader);
    gpu_set_constants(compute.context, GpuStage::compute, 0, 1, constant_buffers);
    gpu_set_srvs(compute.context, GpuStage::compute, 0, view_count, views);
    gpu_set_uavs(compute.context, 0, 2, uavs);
    gpu_dispatch(compute.context, groups_x, groups_y, 1);
    GpuSrv* no_views[2] = {};
    GpuUav* no_uavs[2] = {};
    gpu_set_srvs(compute.context, GpuStage::compute, 0, view_count, no_views);
    gpu_set_uavs(compute.context, 0, 2, no_uavs);
    gpu_release(buffer);
    return true;
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        check(false, "device creation");
        return;
    }
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));
    check(gpu_ok(gpu_format_check(device, GpuFormat::rgba8, gpu_bind_srv | gpu_bind_rtv | gpu_bind_uav)),
          "an RGBA8 atlas can be sampled, rendered to and written by compute");
    check(gpu_caps(device).max_compute_invocations >= 64, "the compute group fits an 8 x 8 tile");
    check(gpu_caps(device).max_compute_storage_buffers >= 3, "a decode dispatch binds its storage buffers");

    GpuComputeShader *png = nullptr, *vp8 = nullptr, *jpeg_idct = nullptr, *jpeg_atlas = nullptr;
    check(gpu_ok(gpu_compute_shader_create(device, ShaderId::png_compute, &png)) &&
              gpu_ok(gpu_compute_shader_create(device, ShaderId::vp8_atlas, &vp8)) &&
              gpu_ok(gpu_compute_shader_create(device, ShaderId::jpeg_idct, &jpeg_idct)) &&
              gpu_ok(gpu_compute_shader_create(device, ShaderId::jpeg_atlas, &jpeg_atlas)),
          "the decode shaders");

    GpuTexture* atlas = nullptr;
    GpuSrv* atlas_view = nullptr;
    GpuRtv* atlas_target = nullptr;
    GpuUav* atlas_uav = nullptr;
    check(gpu_ok(gpu_texture_create(device,
                                    {media_scene::kAtlasWidth, media_scene::kAtlasHeight, GpuFormat::rgba8,
                                     GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv | gpu_bind_uav},
                                    nullptr, &atlas)) &&
              gpu_ok(gpu_srv_create(device, atlas, {GpuViewKind::texture, GpuFormat::rgba8}, &atlas_view)) &&
              gpu_ok(gpu_rtv_create(device, atlas, &atlas_target)) && gpu_ok(gpu_uav_create(device, atlas, &atlas_uav)),
          "the atlas");

    media_scene::build(&g_data);
    GpuBuffer* palette = nullptr;
    GpuSrv* palette_view = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(g_data.palette), GpuUsage::default_, gpu_bind_srv, false},
                                   g_data.palette, &palette)) &&
              gpu_ok(gpu_srv_create(device, palette, {GpuViewKind::buffer_typed, GpuFormat::r32_uint, sizeof(g_data.palette) / 4},
                                    &palette_view)),
          "the palette");
    RawInput yuv{}, alpha{}, coefficients{}, quantization{};
    check(make_input(device, context, g_data.yuv, sizeof(g_data.yuv), &yuv) &&
              make_input(device, context, g_data.vp8_alpha, sizeof(g_data.vp8_alpha), &alpha) &&
              make_input(device, context, g_data.jpeg_coefficients, sizeof(g_data.jpeg_coefficients), &coefficients) &&
              make_input(device, context, g_data.jpeg_quantization, sizeof(g_data.jpeg_quantization), &quantization),
          "the VP8 and JPEG inputs");
    // The JPEG samples: written by the inverse transform, read by the atlas passes.
    GpuBuffer* samples = nullptr;
    GpuSrv* sample_view = nullptr;
    GpuUav* sample_uav = nullptr;
    check(gpu_ok(gpu_buffer_create(device,
                                   {media_scene::kJpegSamples * 4, GpuUsage::default_, gpu_bind_srv | gpu_bind_uav, true},
                                   nullptr, &samples)) &&
              gpu_ok(gpu_srv_create(device, samples, {GpuViewKind::buffer_raw, GpuFormat::r32_uint, media_scene::kJpegSamples},
                                    &sample_view)) &&
              gpu_ok(gpu_uav_create(device, samples, media_scene::kJpegSamples, &sample_uav)),
          "the JPEG sample buffer");

    GpuTexture* target = nullptr;
    GpuRtv* view = nullptr;
    check(gpu_ok(gpu_texture_create(device,
                                    {media_scene::kFrameWidth, media_scene::kFrameHeight, GpuFormat::rgba8,
                                     GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv},
                                    nullptr, &target)) &&
              gpu_ok(gpu_rtv_create(device, target, &view)),
          "render target");
    GpuTexture* side[2] = {};
    GpuSrv* side_views[2] = {};
    for (u32 i = 0; i < 2; ++i) {
        media_scene::side_picture(i, g_side[i]);
        const GpuInitialData initial{g_side[i], 64};
        check(gpu_ok(gpu_texture_create(device, {16, 16, GpuFormat::rgba8, GpuUsage::immutable, gpu_bind_srv}, &initial,
                                        &side[i])) &&
                  gpu_ok(gpu_srv_create(device, side[i], {GpuViewKind::texture, GpuFormat::rgba8}, &side_views[i])),
              "a side atlas");
    }
    GpuBuffer* instances = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(media_scene::kQuads), GpuUsage::dynamic, gpu_bind_vertex, false},
                                   nullptr, &instances)),
          "instance buffer");
    GpuBuffer* frame = nullptr;
    check(gpu_ok(gpu_buffer_create(device, {sizeof(media_scene::kFrame), GpuUsage::default_, gpu_bind_constant, false},
                                   &media_scene::kFrame, &frame)),
          "frame constants");
    // The elements of D3D11DrawPipeline's media layout (draw_pipeline.cpp).
    const GpuInputElement elements[] = {{"RECT", 0, 0}, {"TEXCOORD", 0, 16}, {"TEXCOORD", 1, 32}, {"TEXCOORD", 2, 48}};
    GpuVertexShader* vertex_shader = nullptr;
    GpuInputLayout* layout = nullptr;
    bool layout_failed = false;
    check(gpu_ok(gpu_vertex_shader_create(device, ShaderId::media_vertex, {elements, 4}, &vertex_shader, &layout,
                                          &layout_failed)),
          "media vertex shader");
    GpuPixelShader* pixel_shader = nullptr;
    check(gpu_ok(gpu_pixel_shader_create(device, ShaderId::media_pixel, &pixel_shader)), "media pixel shader");
    GpuSampler* sampler = nullptr;
    check(gpu_ok(gpu_sampler_create(device, {GpuFilter::linear, GpuAddress::clamp}, &sampler)), "media sampler");
    GpuBlend* blend = nullptr;
    check(gpu_ok(gpu_blend_create(device,
                                  {true, GpuBlendFactor::one, GpuBlendFactor::inv_src_alpha, GpuBlendFactor::one,
                                   GpuBlendFactor::inv_src_alpha},
                                  &blend)),
          "premultiplied blend");
    GpuRaster* raster = nullptr;
    check(gpu_ok(gpu_raster_create(device, &raster)), "rasterizer");

    if (g_failures == 0) {
        const float transparent[4] = {0, 0, 0, 0};
        gpu_clear_rtv(context, atlas_target, transparent);
        const Compute compute{device, context, atlas_uav};
        bool dispatched = true;
        for (u32 job = 0; job < media_scene::kPngJobs; ++job) {
            const media_scene::PngConstants constants = media_scene::png_job(job);
            RawInput scanlines{};
            if (!make_input(device, context, media_scene::png_bytes(g_data, job), media_scene::png_byte_count(job),
                            &scanlines)) {
                dispatched = false;
                break;
            }
            GpuSrv* views[2] = {scanlines.view, palette_view};
            dispatched = dispatch(compute, png, &constants, sizeof(constants), 2, views, atlas_uav,
                                  groups(constants.image_width), groups(constants.image_height)) &&
                         dispatched;
            release_input(&scanlines);
        }
        check(dispatched, "the PNG dispatches (the second over-blends onto the first)");
        dispatched = true;
        for (u32 job = 0; job < media_scene::kVp8Jobs; ++job) {
            const media_scene::Vp8Constants constants = media_scene::vp8_job(job);
            GpuSrv* views[2] = {yuv.view, alpha.view};
            dispatched = dispatch(compute, vp8, &constants, sizeof(constants), 2, views, atlas_uav,
                                  groups(media_scene::kVp8Width), groups(media_scene::kVp8Height)) &&
                         dispatched;
        }
        check(dispatched, "the VP8 dispatches");
        {
            const media_scene::JpegIdctConstants constants = media_scene::jpeg_idct_constants();
            GpuSrv* views[2] = {coefficients.view, quantization.view};
            dispatched = dispatch(compute, jpeg_idct, &constants, sizeof(constants), 2, views, sample_uav,
                                  media_scene::kJpegBlocks, 1);
        }
        // The inverse transform wrote the samples through a storage buffer the atlas passes read: the backend orders them.
        for (const media_scene::JpegJob& job : media_scene::kJpegJobs) {
            const media_scene::JpegAtlasConstants constants =
                media_scene::jpeg_atlas_constants(job.destination_x, job.destination_y, job.orientation);
            GpuSrv* views[1] = {sample_view};
            dispatched = dispatch(compute, jpeg_atlas, &constants, sizeof(constants), 1, views, atlas_uav,
                                  groups(media_scene::kJpegSize), groups(media_scene::kJpegSize)) &&
                         dispatched;
        }
        check(dispatched, "the JPEG dispatches");
        check(gpu_ok(gpu_readback(device, context, atlas, {0, 0, media_scene::kAtlasWidth, media_scene::kAtlasHeight},
                                  g_atlas, media_scene::kAtlasWidth * 4)),
              "atlas readback");

        void* mapped = gpu_map(context, instances, GpuMap::discard, nullptr);
        check(mapped != nullptr, "instance map");
        if (mapped) {
            copy_bytes(mapped, media_scene::kQuads, sizeof(media_scene::kQuads));
            gpu_unmap(context, instances);
        }
        gpu_clear_rtv(context, view, media_scene::kClear);
        gpu_set_render_target(context, view);
        gpu_set_viewport(context, 0, 0, static_cast<float>(media_scene::kFrameWidth),
                         static_cast<float>(media_scene::kFrameHeight));
        gpu_set_scissor(context, {0, 0, static_cast<i32>(media_scene::kFrameWidth),
                                  static_cast<i32>(media_scene::kFrameHeight)});
        gpu_set_raster(context, raster);
        const float no_factor[4] = {0, 0, 0, 0};
        gpu_set_blend(context, blend, no_factor);
        GpuBuffer* constants[1] = {frame};
        gpu_set_constants(context, GpuStage::vertex, 0, 1, constants);
        GpuSrv* views[3] = {atlas_view, side_views[0], side_views[1]};
        gpu_set_srvs(context, GpuStage::pixel, 0, 3, views);
        GpuSampler* samplers[1] = {sampler};
        gpu_set_samplers(context, GpuStage::pixel, 0, 1, samplers);
        gpu_set_input_layout(context, layout);
        gpu_set_vertex_buffer(context, instances, sizeof(media_scene::Quad), 0);
        gpu_set_shader(context, vertex_shader);
        gpu_set_shader(context, pixel_shader);
        gpu_draw_instanced(context, 4, media_scene::kQuadCount, 0, 0);
        check(gpu_ok(gpu_readback(device, context, target, {0, 0, media_scene::kFrameWidth, media_scene::kFrameHeight},
                                  g_frame, media_scene::kFrameWidth * 4)),
              "frame readback");

        const char* dump_prefix = environment_get("MAICORD_GPU_MEDIA_DUMP");
        if (dump_prefix && dump_prefix[0]) {
            dump(dump_prefix, ".atlas", g_atlas, sizeof(g_atlas));
            dump(dump_prefix, ".draw", g_frame, sizeof(g_frame));
        }
        const bool decoded = png_golden::decode({kAtlasGolden, sizeof(kAtlasGolden)}, media_scene::kAtlasWidth,
                                                media_scene::kAtlasHeight, 4, g_atlas_golden) &&
                             png_golden::decode({kDrawGolden, sizeof(kDrawGolden)}, media_scene::kFrameWidth,
                                                media_scene::kFrameHeight, 4, g_frame_golden);
        check(decoded, "the golden PNGs decode");
        if (decoded) {
            const png_golden::Difference atlas_difference =
                png_golden::compare(g_atlas, g_atlas_golden, media_scene::kAtlasWidth, media_scene::kAtlasHeight, 4,
                                    media_scene::kAtlasTolerance.channel);
            test_write_metric("atlas maximum channel difference=", atlas_difference.maximum);
            test_write_metric("atlas pixels over tolerance=", atlas_difference.over_tolerance);
            if (atlas_difference.over_tolerance) {
                test_write_metric("first atlas pixel x=", atlas_difference.first_x);
                test_write_metric("first atlas pixel y=", atlas_difference.first_y);
            }
            check(png_golden::within(atlas_difference, media_scene::kAtlasTolerance),
                  "the atlas matches tests/gpu/golden/media_atlas.png");
            const png_golden::Difference frame_difference =
                png_golden::compare(g_frame, g_frame_golden, media_scene::kFrameWidth, media_scene::kFrameHeight, 4,
                                    media_scene::kFrameTolerance.channel);
            test_write_metric("frame maximum channel difference=", frame_difference.maximum);
            test_write_metric("frame pixels over tolerance=", frame_difference.over_tolerance);
            if (frame_difference.over_tolerance) {
                test_write_metric("first frame pixel x=", frame_difference.first_x);
                test_write_metric("first frame pixel y=", frame_difference.first_y);
            }
            check(png_golden::within(frame_difference, media_scene::kFrameTolerance),
                  "the frame matches tests/gpu/golden/media_draw.png");
        }
    }
    gpu_release(raster);
    gpu_release(blend);
    gpu_release(sampler);
    gpu_release(pixel_shader);
    gpu_release(layout);
    gpu_release(vertex_shader);
    gpu_release(frame);
    gpu_release(instances);
    for (u32 i = 0; i < 2; ++i) {
        gpu_release(side_views[i]);
        gpu_release(side[i]);
    }
    gpu_release(view);
    gpu_release(target);
    gpu_release(sample_uav);
    gpu_release(sample_view);
    gpu_release(samples);
    release_input(&quantization);
    release_input(&coefficients);
    release_input(&alpha);
    release_input(&yuv);
    gpu_release(palette_view);
    gpu_release(palette);
    gpu_release(atlas_uav);
    gpu_release(atlas_target);
    gpu_release(atlas_view);
    gpu_release(atlas);
    gpu_release(jpeg_atlas);
    gpu_release(jpeg_idct);
    gpu_release(vp8);
    gpu_release(png);
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_media_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_media");
}
